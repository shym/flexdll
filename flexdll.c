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
#if defined(_M_AMD64) && !defined(MSVC64)
#error 64-bit cl selected for a 32-bit build
#elif !defined(_M_AMD64) && defined(MSVC64)
#error 32-bit cl selected for a 64-bit build
#endif
#endif /* MSVC */

typedef long intnat;
typedef unsigned long uintnat;

#define RELOC_REL32     0x0001
#define RELOC_ABS       0x0002
#define RELOC_REL32_4   0x0003
#define RELOC_REL32_1   0x0004
#define RELOC_REL32_2   0x0005
#define RELOC_DONE      0x0100

typedef struct { UINT_PTR kind; char *name; UINT_PTR *addr; } reloc_entry;
typedef struct { char *first; char *last; DWORD old; } nonwr;
typedef struct { nonwr *nonwr; reloc_entry entries[]; } reloctbl;
typedef struct { void *addr; char *name; } symtbl_entry;
typedef struct { void *addr; char *name; void *trampoline; } dynsymbol;
typedef struct { UINT_PTR size; symtbl_entry entries[]; } raw_symtbl;
typedef struct { UINT_PTR size; dynsymbol entries[]; } symtbl;
typedef struct dlunit {
  void *handle;
  symtbl *symtbl;
  int global;
  int count;
  struct dlunit *next,*prev;
} dlunit;
typedef dynsymbol *resolver(void*, const char*);

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

static void allow_write(char *begin, char *end, DWORD new, PDWORD old) {
  static long int pagesize = 0;
  int res;
  SYSTEM_INFO si;

  if (0 == pagesize) {
    GetSystemInfo (&si);
    pagesize = si.dwPageSize;
  }

  begin -= (size_t) begin % pagesize;
  res = VirtualProtect(begin, end - begin, new, old);
  if (0 == res) {
    fprintf(stderr, "natdynlink: VirtualProtect failed (%s), begin = 0x%p, end = 0x%p, new = %x\n", ll_dlerror(), begin, end, new);
    exit(2);
  }
  /* printf("%p -> %p\n", *old, new); */
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

static void relocate(resolver f, void *data, reloctbl *tbl, void **jmptbl) {
  reloc_entry *ptr;
  nonwr *wr;
  INT_PTR s;
  dynsymbol *sym;
  int rel_offset;
  char* reloc_type;
  /*
  DWORD old;
  MEMORY_BASIC_INFORMATION info;
  */

  if (!tbl) return;

  for (wr = tbl->nonwr; wr->last != 0; wr++)
    allow_write(wr->first,wr->last + sizeof(UINT_PTR),PAGE_EXECUTE_WRITECOPY,&wr->old);

  for (ptr = tbl->entries; ptr->kind; ptr++) {
    if (ptr->kind & RELOC_DONE) continue;

    /*
    assert(VirtualQuery(ptr->addr, &info, sizeof(info)) == sizeof(info));
    printf("p = %p, base = %p, allocBase = %p, allocProtect = %x, state = %x, protect = %x, type = %x\n",  ptr->addr, info.BaseAddress, info.AllocationBase, info.AllocationProtect, info.State, info.Protect, info.Type);

    allow_write(ptr->addr,ptr->addr+4,PAGE_EXECUTE_WRITECOPY,&old);

    assert(VirtualQuery(ptr->addr, &info, sizeof(info)) == sizeof(info));
    printf("p = %p, base = %p, allocBase = %p, allocProtect = %x, state = %x, protect = %x, type = %x\n",  ptr->addr, info.BaseAddress, info.AllocationBase, info.AllocationProtect, info.State, info.Protect, info.Type);
    */


    sym = f(data, ptr->name);
    if (!sym) {
      error = 2;
      cannot_resolve_msg(ptr->name);
      return;
    }
    s = (UINT_PTR)sym->addr;

    switch (ptr->kind & 0xff) {
    case RELOC_ABS:
      rel_offset = 0;
      break;
    case RELOC_REL32:
      rel_offset = 4;
      reloc_type = "";
      break;
    case RELOC_REL32_4:
      rel_offset = 8;
      reloc_type = "_4";
      break;
    case RELOC_REL32_1:
      rel_offset = 5;
      reloc_type = "_1";
      break;
    case RELOC_REL32_2:
      rel_offset = 6;
      reloc_type = "_2";
      break;
    default:
      fprintf(stderr, "flexdll: unknown relocation kind");
      exit(2);
    }

    if (!rel_offset) {
      *(ptr->addr) += s;
    } else {
      s -= (INT_PTR)(ptr -> addr) + rel_offset;
      s += *((INT32*) ptr -> addr);
      if (s != (INT32) s) {
        if (jmptbl) {
          if (!sym->trampoline) {
            void* trampoline = sym->trampoline = *jmptbl;
            /* movq $(sym->addr), %rax */
            *((short*)trampoline) = 0xb848;
            *((UINT_PTR*)((char*)trampoline + 2)) = (UINT_PTR)sym->addr;
            /* jmp %rax */
            *((short*)((char*)trampoline + 10)) = 0xe0ff;
            *((char*)jmptbl) += 16;
          }
          s = (UINT_PTR)(sym->trampoline);
          s -= (INT_PTR)(ptr->addr) + rel_offset;
          s += *((INT32*)ptr->addr);
        } else {
          sprintf(error_buffer, "flexdll error: cannot relocate RELOC_REL32%s, target is too far: %p  %p",reloc_type,(void *)((UINT_PTR) s), (void *) ((UINT_PTR)(INT32) s));
          error = 3;
          return;
        }
      }
      *((UINT32*) ptr->addr) = s;
    }
    ptr->kind |= RELOC_DONE;

    /*
    allow_write(ptr->addr,ptr->addr+4,old,&old);
    assert(VirtualQuery(ptr->addr, &info, sizeof(info)) == sizeof(info));
    printf("p = %p, base = %p, allocBase = %p, allocProtect = %x, state = %x, protect = %x, type = %x\n",  ptr->addr, info.BaseAddress, info.AllocationBase, info.AllocationProtect, info.State, info.Protect, info.Type);
    */
  }

  /* Restore permissions. Should do it also in case of failure... */
  for (wr = tbl->nonwr; wr->last != 0; wr++)
    allow_write(wr->first,wr->last + 4,wr->old,&wr->old);
}

static void relocate_master(resolver f, void *data, reloctbl **ptr, void *jmptbl) {
  void **pjmptbl = jmptbl ? &jmptbl : NULL;
  while (0 == error && *ptr)
    relocate(f, data, *ptr++, pjmptbl);
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

static dynsymbol *find_symbol(symtbl *tbl, const char *name) {
  static dynsymbol s;
  dynsymbol *sym;

  if (!tbl) return NULL;

  s.name = (char*) name;
  sym =
    bsearch(&s,&tbl->entries,tbl->size, sizeof(dynsymbol),&compare_dynsymbol);

  return sym;
}



/* API */

extern raw_symtbl static_symtable;
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

static symtbl *augment_symtbl(raw_symtbl *raw_symtbl) {
  symtbl *result;
  dynsymbol *ptr;
  symtbl_entry *src;
  int i;
  result = (symtbl*)malloc(raw_symtbl->size * sizeof(dynsymbol) + sizeof(UINT_PTR));
  ptr = result->entries;
  src = raw_symtbl->entries;
  i = result->size = raw_symtbl->size;
  while (i-- > 0) {
    ptr->addr = src->addr;
    ptr->name = (src++)->name;
    (ptr++)->trampoline = NULL;
  }
  return result;
}

static symtbl *get_static_symtable(void) {
  static symtbl *table = NULL;

  if (table)
    return table;
  else
    return (table = augment_symtbl(&static_symtable));
}

static dynsymbol *find_symbol_global(void *data, const char *name) {
  dynsymbol *sym;
  dlunit *unit;

  if (!name) return NULL;
  sym = find_symbol(get_static_symtable(), name);
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

int flexdll_relocate_v2(void *tbl, void *jmptbl) {
  if (!tbl) { printf("No master relocation table\n"); return 0; }
  relocate_master(find_symbol_global, NULL, tbl, jmptbl);
  if (error) return 0;
  return 1;
}

int flexdll_relocate(void *tbl) {return flexdll_relocate_v2(tbl, NULL);}

void set_env_ptr(char* name, void* ptr) {
  char env[256];

#if defined(CYGWIN) || __STDC_SECURE_LIB__ >= 200411L
  sprintf(env, "%p", ptr);
#endif

#ifdef CYGWIN
  setenv(name, env, 1);
#elif __STDC_SECURE_LIB__ >= 200411L
  _putenv_s(name, env);
#else
  {
    char* s;
    sprintf(env, "%s=%p", name, relocate);
    s = malloc(strlen(env) + 1);
    strcpy(s, env);
    putenv(s);
  }
#endif
}

#ifdef CYGWIN
void *flexdll_dlopen(const char *file, int mode) {
#else
void *flexdll_wdlopen(const wchar_t *file, int mode) {
#endif
  void *handle;
  dlunit *unit;
  int exec = (mode & FLEXDLL_RTLD_NOEXEC ? 0 : 1);

  error = 0;
  if (!file) return &main_unit;

  set_env_ptr("FLEXDLL_RELOCATE", (exec ? &flexdll_relocate : 0));
  set_env_ptr("FLEXDLL_RELOCATE_V2", (exec ? &flexdll_relocate_v2 : 0));

  handle = ll_dlopen(file, exec);
  if (!handle) { if (!error) error = 1; return NULL; }

  unit = units;
  while ((NULL != unit) && (unit->handle != handle)) unit = unit->next;
  if (unit) { unit->count++; }
  else {
    unit = malloc(sizeof(dlunit));
    unit->handle = handle;
    unit->symtbl = augment_symtbl(ll_dlsym(handle, "symtbl"));
    unit->count = 1;
    unit->global = 0;
    push_unit(unit);
  }
  if (mode & FLEXDLL_RTLD_GLOBAL) unit->global=1;

  if (exec) {
    /* Relocation has already been done if the flexdll's DLL entry point
       is used */
    flexdll_relocate_v2(ll_dlsym(handle, "reloctbl"), ll_dlsym(handle, "jmptbl"));
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
  if (unit->count == 0) { unlink_unit(unit); free(unit->symtbl); free(unit); }
}


void *flexdll_dlsym(void *u, const char *name) {
  dynsymbol *result;
  if (u == &main_unit) result = find_symbol_global(NULL, name);
  else if (u == NULL) result = find_symbol(get_static_symtable(), name);
  else result = find_symbol(((dlunit*)u)->symtbl, name);
  return (result ? result->addr : NULL);
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
  if (NULL == u) { dump_symtbl(get_static_symtable()); }
  else if (u == &main_unit) {
    dump_symtbl(get_static_symtable());
    for (unit = units; unit; unit = unit->next)
      if (unit->global) { dump_symtbl(unit->symtbl); }
  }
  else { dump_symtbl(unit->symtbl); }
}

void flexdll_dump_relocations(void *u) {
  if (NULL == u || u == &main_unit) return;
  dump_master_reloctbl(ll_dlsym(((dlunit*)u) -> handle, "reloctbl"));
}
