/*****************************************************************
   FlexDLL
   Alain Frisch

   Copyright 2007 Institut National de Recherche en Informatique et
   en Automatique.

******************************************************************/

/* Runtime support library */

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "flexdll.h"

/* Guard against compiling with the wrong cl! */
#ifdef MSVC
#if defined(_M_AMD64) && (!defined(MSVC64) || defined(MSVCARM64))
#error Wrong flags specified for an x86_64 cl
#elif defined(_M_ARM64) && (!defined(MSVCARM64) || defined(MSVC64))
#error Wrong flags specified for an aarch64 cl
#elif defined(_M_IX86) && (defined(MSVC64) || defined(MSVCARM64))
#error Wrong flags specified for an x86 cl
#endif
#endif /* MSVC */

typedef long intnat;
typedef unsigned long uintnat;

#define RELOC_REL32     0x0001
#define RELOC_ABS       0x0002
#define RELOC_REL32_4   0x0003
#define RELOC_REL32_1   0x0004
#define RELOC_REL32_2   0x0005
#define RELOC_PAGEOFFSET_12A 0x0006
#define RELOC_PAGEOFFSET_12L 0x0007
#define RELOC_PAGEBASE_REL21 0x0008
#define RELOC_BRANCH14 0x0009
#define RELOC_BRANCH19 0x000a
#define RELOC_BRANCH26 0x000b
#define RELOC_DONE      0x0100

typedef struct { UINT_PTR kind; char *name; UINT_PTR *addr; } reloc_entry;
typedef struct { char *first; char *last; DWORD old; } nonwr;
typedef struct { nonwr *nonwr; reloc_entry entries[]; } reloctbl;
typedef struct { void *addr; char *name; } dynsymbol;
typedef struct { UINT_PTR size; dynsymbol entries[]; } symtbl;
typedef struct dlunit {
  void *handle;
  symtbl *symtbl;
  int global;
  int count;
  struct dlunit *next,*prev;
} dlunit;
typedef void *resolver(void*, const char*);

static int error = 0;
static char error_buffer[256];

/* Emulate a low-level dlopen-like interface */

#ifdef __CYGWIN__

/* Under Cygwin, use the dlopen interface to allow POSIX paths */

#include <dlfcn.h>

static void *ll_dlopen(const char *libname, int for_execution) {
  return dlopen(libname, RTLD_NOW | RTLD_GLOBAL);
  /* Could use RTLD_LAZY if for_execution == 0, but needs testing */
}

static void ll_dlclose(void * handle)
{
  dlclose(handle);
}

static void * ll_dlsym(void * handle, char * name)
{
  return dlsym(handle, name);
}

static char * ll_dlerror(void)
{
  return dlerror();
}

#else

static void *ll_dlopen(const wchar_t *libname, int for_execution) {
  HMODULE m;
  m = LoadLibraryExW(libname, NULL,
                     for_execution ? 0 : DONT_RESOLVE_DLL_REFERENCES);
  /* See https://blogs.msdn.microsoft.com/oldnewthing/20050214-00/?p=36463
     Should use LOAD_LIBRARY_AS_DATAFILE instead of DONT_RESOLVE_DLL_REFERENCES? */

  /* Under Win 95/98/ME, LoadLibraryEx can fail in cases where LoadLibrary
     would succeed.  Just try again with LoadLibrary for good measure. */
  if (m == NULL) m = LoadLibraryW(libname);
  /*fprintf(stderr, "Loaded %S at %p\n", libname, m); fflush(stderr);*/
  return (void *) m;
}


static void ll_dlclose(void *handle) {
  FreeLibrary((HMODULE) handle);
}

static void *ll_dlsym(void *handle, char *name) {
  return (void *) GetProcAddress((HMODULE) handle, name);
}

static char *ll_dlerror(void)
{
  DWORD msglen =
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,           /* message source */
                  GetLastError(), /* error number */
                  0,              /* default language */
                  error_buffer,         /* destination */
                  sizeof(error_buffer), /* size of destination */
                  NULL);          /* no inserts */
  if (msglen == 0) return "unknown error";
  else return error_buffer;
}

#endif

/** Relocation tables **/

static void dump_reloctbl(reloctbl *tbl) {
  reloc_entry *ptr;
  nonwr *wr;

  if (!tbl) { printf("No relocation table\n"); return; }
  printf("Dynamic relocation table found at %p\n", tbl);

  for (wr = tbl->nonwr; wr->last != 0; wr++)
    printf(" Non-writable relocation in zone %p -> %p\n",
	   wr->first,
	   wr->last);

  for (ptr = tbl->entries; ptr->kind; ptr++)
    printf(" %p (kind:%04lx) (now:%p)  %s\n",
	   (void *)ptr->addr,
	   (unsigned long)ptr->kind,
	   (void *)((UINT_PTR)(*((uintnat*) ptr->addr))),
	   ptr->name
	   );
}

static void dump_master_reloctbl(reloctbl **ptr) {
  if (!ptr) return;
  while (*ptr) dump_reloctbl(*ptr++);
}

/* Avoid the use of snprintf */
static void cannot_resolve_msg(char *name) {
  static char msg[] = "Cannot resolve ";
  static int l = sizeof(msg) - 1;
  int n = strlen(name);
  memcpy(error_buffer,msg,l);
  memcpy(error_buffer+l,name,min(n,sizeof(error_buffer) - l - 1));
  error_buffer[l+n] = 0;
}

static void relocate(resolver f, void *data, reloctbl *tbl) {
  reloc_entry *ptr;
  nonwr *wr;
  INT_PTR s;
  DWORD prev_protect;
  static long int page_size = 0;
  SYSTEM_INFO si;
  char *page_start, *page_end;
  char *prev_page_start = (char*)1, *prev_page_end = (char*)1;

  if (!tbl) return;

  if (0 == page_size) {
    GetSystemInfo (&si);
    page_size = si.dwPageSize;
  }

  for (ptr = tbl->entries; ptr->kind; ptr++) {
    if (ptr->kind & RELOC_DONE) continue;

    s = (UINT_PTR) f(data,ptr->name);
    if (!s) {
      error = 2;
      cannot_resolve_msg(ptr->name);
      goto restore;
    }

    /* Set up page protection to allow the relocation.  We will undo
       the change on the next relocation if it falls in a different
       page (or at the end of the process), to avoid too many calls to
       VirtualProtect.

       prev_page_start, prev_page_end, prev_protect: describe the
       protection to be reset.

       Note that a single relocation can fall between two pages.
    */

    page_start = (char*)ptr->addr;
    page_end = page_start+((ptr->kind & 0xff) == RELOC_ABS ? sizeof(UINT_PTR) - 1 : 3);
    page_start -= (size_t) page_start % page_size;
    page_end -= (size_t) page_end % page_size;

    if (prev_page_start != page_start || prev_page_end != page_end) {
      if (prev_page_start != (char*)1) {
        /* Restore */
        if (0 == VirtualProtect(prev_page_start, prev_page_end-prev_page_start+page_size, prev_protect, &prev_protect)) {
          fprintf(stderr, "natdynlink: VirtualProtect failed (%s), page_start = 0x%p\n", ll_dlerror(), page_start);
          exit(2);
        }
      }

      prev_page_start = page_start;
      prev_page_end = page_end;
      if (0 == VirtualProtect(page_start, page_end-page_start+page_size, PAGE_READWRITE, &prev_protect)) {
        fprintf(stderr, "natdynlink: VirtualProtect failed (%s), page_start = 0x%p\n", ll_dlerror(), page_start);
        exit(2);
      }
    }

    /* Perform the relocation.
       s - address of the symbol (provided by the loading process)
       ptr->addr - address of whatever needs patching
     */
    switch (ptr->kind & 0xff) {
    /* - IMAGE_REL_I386_DIR32
       - IMAGE_REL_ARM64_ADDR64
       - IMAGE_REL_AMD64_ADDR64
       Type: pointer
       Patch: add address of s
     */
    case RELOC_ABS:
      /*fprintf(stderr, "RELOC_ABS for %s; symbol at %#018llX and ptr->addr at 0x%p\n", ptr->name, s, (void*)ptr->addr); fflush(stderr);*/
      if (*(ptr->addr)) {fprintf(stderr, "Non-zero RELOC_ABS for %s\n", ptr->name); fflush(stderr);}
      *(ptr->addr) += s;
      break;
    /* - IMAGE_REL_I386_REL32
       - IMAGE_REL_ARM64_REL32
       - IMAGE_REL_AMD64_REL32 / IMAGE_REL_AMD64_REL32_1 / IMAGE_REL_AMD64_REL32_2 / IMAGE_REL_AMD64_REL32_4
       Type: 32bit
       Patch: address of s relative to the nth byte after the 32-bit word addressed by ptr->addr
     */
    case RELOC_REL32:
      fprintf(stderr, "RELOC_REL32 for %s; symbol at %#018llX and ptr->addr at 0x%p\n", ptr->name, s, (void*)ptr->addr); fflush(stderr);
      s -= (INT_PTR)(ptr -> addr) + 4;
      goto patch_rel32;
    case RELOC_REL32_1:
      s -= (INT_PTR)(ptr -> addr) + 5;
      goto patch_rel32;
    case RELOC_REL32_2:
      s -= (INT_PTR)(ptr -> addr) + 6;
      goto patch_rel32;
    case RELOC_REL32_4:
      s -= (INT_PTR)(ptr -> addr) + 8;
patch_rel32:
      /* This debug is expected */
      if (*(ptr->addr)) {fprintf(stderr, "Non-zero RELOC_REL32_* for %s\n", ptr->name); fflush(stderr);}
      s += *((INT32*) ptr -> addr);
      if (s != (INT32) s) {
        /* Could improve error? */
        sprintf(error_buffer, "flexdll error: cannot relocate RELOC_REL32_n, target is too far: %p  %p", (void *)((UINT_PTR) s), (void *) ((UINT_PTR)(INT32) s));
        error = 3;
        goto restore;
      }
      *((UINT32*) ptr->addr) = s;
      break;
    /* - IMAGE_REL_ARM64_PAGEOFFSET_12A
       Type: XXX
       Patch: XXX
     */
    case RELOC_PAGEOFFSET_12A:
      fprintf(stderr, "RELOC_PAGEOFFSET_12A for %s; symbol at %#018llX and ptr->addr at 0x%p\n", ptr->name, s, (void*)ptr->addr); fflush(stderr);
      /* Ensure bits 21:10 zeroed (they should be) */
      *(UINT32*)ptr->addr &= 0xffc003ff;
      /* Put the low 12 bits of s into the instruction */
      *(UINT32*)ptr->addr |= ((s << 10) & 0x3ffc00);
      break;
    /* - IMAGE_REL_ARM64_PAGEOFFSET_12L
       Type: XXX
       Patch: XXX
     */
    case RELOC_PAGEOFFSET_12L:
      fprintf(stderr, "RELOC_PAGEOFFSET_12L for %s; symbol at %#018llX and ptr->addr at 0x%p\n", ptr->name, s, (void*)ptr->addr); fflush(stderr);
      if (s & 0x7) {
        sprintf(error_buffer, "flexdll error: cannot relocate RELOC_PAGEOFFSET_12L, target %s is not aligned: %p", ptr->name, (void *)s);
        error = 3;
        goto restore;
      }
      /* Ensure bits 21:10 zeroed (they should be) */
      *(UINT32*)ptr->addr &= 0xffc003ff;
      /* Put bits 14:3 of s into the instruction */
      *(UINT32*)ptr->addr |= ((s << 7) & 0x3ffc00);
      break;
    /* - IMAGE_REL_ARM64_PAGEBASE_REL21
       Type: XXX
       Patch: XXX
     */
    case RELOC_PAGEBASE_REL21:
      fprintf(stderr, "RELOC_PAGEBASE_REL21 for %s; symbol at %#018llX and ptr->addr at 0x%p\n", ptr->name, s, (void*)ptr->addr); fflush(stderr);
      /* Convert s to 4KiB page address */
      s &= ~0xfff;
      /* Relative address to 4KiB page of PC */
      s -= ((UINT_PTR)ptr->addr & ~0xfff);
      if (s > 0xfffff000 || s < 0xffffffff00000000L) {
        sprintf(error_buffer, "flexdll error: cannot relocate RELOC_PAGEBASE_REL21, target %s is too far: %p", ptr->name, (void *)s);
        error = 3;
        goto restore;
      }
      /* Ensure bits 30:29 and 23:5 zeroed (they should be) */
      *(UINT32*)ptr->addr &= 0x9f00001f;
      *(UINT32*)ptr->addr |= ((*(UINT32*)s << 17) & 0x60000000) | ((*(UINT32*)s >> 7) & 0xffffe0);
      break;
    /*case RELOC_BRANCH14: TODO */
    /*case RELOC_BRANCH19: TODO */
    /* - IMAGE_REL_ARM64_BRANCH26
       Type: 32-bit instruction patch
       Patch: Bits 27:2 of the offset between s and ptr->addr written to bits 25:0 of ptr->addr
     */
    case RELOC_BRANCH26:
      fprintf(stderr, "RELOC_BRANCH26 for %s; symbol at %#018llX and ptr->addr at 0x%p\n", ptr->name, s, (void*)ptr->addr); fflush(stderr);
      s -= (INT_PTR)(ptr -> addr);
      if (s & 0x3) {
        sprintf(error_buffer, "flexdll error: cannot relocate RELOC_BRANCH26, target %s is not aligned: %p", ptr->name, (void *)s);
        error = 3;
        goto restore;
      }
      if (s > 0xffffffc || (INT32)s < 0xf0000000) {
        sprintf(error_buffer, "flexdll error: cannot relocate RELOC_BRANCH26, target %s is too far: %p", ptr->name, (void *)s);
        error = 3;
        goto restore;
        break;
      }
      /* Ensure low 26 bits zeroed (they should be) */
      *(UINT32*)ptr->addr &= 0xfc000000;
      *(UINT32*)ptr->addr |= ((s >> 2) & 0x3ffffff);
      break;
    default:
      fprintf(stderr, "flexdll: unknown relocation kind");
      exit(2);
    }
    ptr->kind |= RELOC_DONE;
  }
 restore:
  /* Restore page permission */
  if (prev_page_start != (char*)1) {
    if (0 == VirtualProtect(prev_page_start, prev_page_end-prev_page_start+page_size, prev_protect, &prev_protect)) {
      fprintf(stderr, "natdynlink: VirtualProtect failed (%s), page_start = 0x%p\n", ll_dlerror(), page_start);
      exit(2);
    }
  }
}

static void relocate_master(resolver f, void *data, reloctbl **ptr) {
  while (0 == error && *ptr) relocate(f,data,*ptr++);
}

/* Symbol tables */

static void dump_symtbl(symtbl *tbl)
{
  int i;

  if (!tbl) { printf("No symbol table\n"); return; }
  printf("Dynamic symbol at %p (size = %u)\n", tbl, (unsigned int) tbl->size); fflush(stdout);

  for (i = 0; i < tbl->size; i++) {
    printf("[%i] ", i); fflush(stdout);
    printf(" %p: ", tbl->entries[i].addr); fflush(stdout);
    printf("%s\n", tbl->entries[i].name);
    fflush(stdout);
  }
}

static int compare_dynsymbol(const void *s1, const void *s2) {
  return strcmp(((dynsymbol*) s1) -> name, ((dynsymbol*) s2) -> name);
}

static void *find_symbol(symtbl *tbl, const char *name) {
  static dynsymbol s;
  dynsymbol *sym;

  if (!tbl) return NULL;

  s.name = (char*) name;
  sym =
    bsearch(&s,&tbl->entries,tbl->size, sizeof(dynsymbol),&compare_dynsymbol);

  return (NULL == sym ? NULL : sym -> addr);
}



/* API */

extern symtbl static_symtable;
static dlunit *units = NULL;
static dlunit main_unit;

static void push_unit(dlunit *unit) {
  unit->next = units;
  unit->prev = NULL;
  if (units) units->prev = unit;
  units = unit;
}

static void unlink_unit(dlunit *unit) {
  if (unit->prev) unit->prev->next=unit->next;
  else units=unit->next;

  if (unit->next) unit->next->prev=unit->prev;
}

static void *find_symbol_global(void *data, const char *name) {
  void *sym;
  dlunit *unit;

  if (!name) return NULL;
  sym = find_symbol(&static_symtable, name);
  if (sym) return sym;

  for (unit = units; unit; unit = unit->next) {
    if (unit->global) {
      sym = find_symbol(unit->symtbl, name);
      if (sym) {
	if (unit != units) { unlink_unit(unit); push_unit(unit); }
	return sym;
      }
    }
  }
  return NULL;
}

int flexdll_relocate(void *tbl) {
  if (!tbl) { printf("No master relocation table\n"); return 0; }
  relocate_master(find_symbol_global, NULL, tbl);
  if (error) return 0;
  return 1;
}

#ifdef CYGWIN
void *flexdll_dlopen(const char *file, int mode) {
#else
void *flexdll_wdlopen(const wchar_t *file, int mode) {
#endif
  void *handle;
  dlunit *unit;
  char flexdll_relocate_env[256];
  int exec = (mode & FLEXDLL_RTLD_NOEXEC ? 0 : 1);
  void* relocate = (exec ? &flexdll_relocate : 0);

  error = 0;
  if (!file) return &main_unit;

#ifdef CYGWIN
  sprintf(flexdll_relocate_env,"%p",relocate);
  setenv("FLEXDLL_RELOCATE", flexdll_relocate_env, 1);
#else
#if __STDC_SECURE_LIB__ >= 200411L
  sprintf(flexdll_relocate_env,"%p",relocate);
  _putenv_s("FLEXDLL_RELOCATE", flexdll_relocate_env);
#else
  {
    char* s;
    sprintf(flexdll_relocate_env,"FLEXDLL_RELOCATE=%p",relocate);
    s = malloc(strlen(flexdll_relocate_env) + 1);
    strcpy(s, flexdll_relocate_env);
    putenv(s);
  }
#endif /* __STDC_SECURE_LIB__ >= 200411L*/
#endif /* CYGWIN */

  handle = ll_dlopen(file, exec);
  if (!handle) { if (!error) error = 1; return NULL; }

  unit = units;
  while ((NULL != unit) && (unit->handle != handle)) unit = unit->next;
  if (unit) { unit->count++; }
  else {
    unit = malloc(sizeof(dlunit));
    unit->handle = handle;
    unit->symtbl = ll_dlsym(handle, "symtbl");
    unit->count = 1;
    unit->global = 0;
    push_unit(unit);
  }
  if (mode & FLEXDLL_RTLD_GLOBAL) unit->global=1;

  if (exec) {
    /* Relocation has already been done if the flexdll's DLL entry point
       is used */
    flexdll_relocate(ll_dlsym(handle, "reloctbl"));
    if (error) { flexdll_dlclose(unit); return NULL; }
  }

  return unit;
}

#ifndef CYGWIN

void *flexdll_dlopen(const char *file, int mode)
{
  wchar_t * p;
  int nbr;
  void * handle;

  nbr = MultiByteToWideChar(CP_THREAD_ACP, 0, file, -1, NULL, 0);
  if (nbr == 0) { if (!error) error = 1; return NULL; }
  p = malloc(nbr*sizeof(*p));
  MultiByteToWideChar(CP_THREAD_ACP, 0, file, -1, p, nbr);
  handle = flexdll_wdlopen(p, mode);
  free(p);

  return handle;
}

#endif

void flexdll_dlclose(void *u) {
  dlunit *unit = u;

  if (NULL == u || u == &main_unit) return;
  ll_dlclose(unit->handle);
  unit->count--;
  if (unit->count == 0) { unlink_unit(unit); free(unit); }
}


void *flexdll_dlsym(void *u, const char *name) {
  if (u == &main_unit) return find_symbol_global(NULL,name);
  else if (NULL == u) return find_symbol(&static_symtable,name);
  else return find_symbol(((dlunit*)u)->symtbl,name);
}

char *flexdll_dlerror() {
  switch (error) {
  case 0: return NULL;
  case 1: error = 0; return ll_dlerror();
  case 2: error = 0; return error_buffer;
  case 3: error = 0; return error_buffer;
  }
  return NULL;
}

void flexdll_dump_exports(void *u) {
  dlunit *unit = u;
  if (NULL == u) { dump_symtbl(&static_symtable); }
  else if (u == &main_unit) {
    dump_symtbl(&static_symtable);
    for (unit = units; unit; unit = unit->next)
      if (unit->global) { dump_symtbl(unit->symtbl); }
  }
  else { dump_symtbl(unit->symtbl); }
}

void flexdll_dump_relocations(void *u) {
  if (NULL == u || u == &main_unit) return;
  dump_master_reloctbl(ll_dlsym(((dlunit*)u) -> handle, "reloctbl"));
}
