#!/bin/bash

OCAMLROOT=$(echo "$OCAMLROOT" | cygpath -m -f -)

function run {
    NAME=$1
    shift
    echo "-=-=- $NAME -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-"
    "$@"
    CODE=$?
    if [ $CODE -ne 0 ]; then
        echo "-=-=- $NAME failed! -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-"
        exit $CODE
    else
        echo "-=-=- End of $NAME -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-"
    fi
}

function configure_ocaml {
  if ((CYGWIN_BUILD)) ; then
    if [ "$1" = "full" ] ; then
      ./configure
    else
      ./configure -no-ocamldoc -no-debugger $NO_ALT_RUNTIMES
      sed -i -e "s|OTHERLIBRARIES=.*/OTHERLIBRARIES=|" config/Makefile
    fi
  else
    cp config/m-nt.h $HEADER_DIR/m.h
    cp config/s-nt.h $HEADER_DIR/s.h

    if [ "$1" = "full" ] ; then
      DISABLE=()
    else
      DISABLE=(-e "s/\(OTHERLIBRARIES\|WITH_OCAMLDOC\|WITH_DEBUGGER\)=.*/\1=/")
    fi

    sed -e "s|PREFIX=.*|PREFIX=$OCAMLROOT|" \
        "${DISABLE[@]}" \
        config/Makefile.$OCAML_PORT > $CONFIG_DIR/Makefile
    #run "Content of config/Makefile" cat $CONFIG_DIR/Makefile
  fi
}

MAKEOCAML=make
CONFIG_DIR=config
HEADER_DIR=byterun/caml
FLEXDLL_BOOTSTRAP=flexdll
CYGWIN_BUILD=0
NO_ALT_RUNTIMES="-no-instrumented-runtime -no-debug-runtime"

case $OCAMLBRANCH in
  4.03|4.04)
    MAKEOCAML="make -f Makefile.nt"
    HEADER_DIR=config
    NO_ALT_RUNTIMES=
  ;;
  4.05)
    HEADER_DIR=config
    NO_ALT_RUNTIMES=
  ;;
  4.06)
    NO_ALT_RUNTIMES=
  ;;
esac

case $OCAML_PORT in
  cygwin*)
    FLEXDLL_BOOTSTRAP=
    CYGWIN_BUILD=1
  ;;
esac

function check_ocaml {
  # Bootstrapping flexlink fails if flexlink is in PATH prior to 4.04.1
  if [ $OCAMLBRANCH = "4.03" ] ; then
    sed -i -e "s/:=.*/:=/" config/Makefile.m*
  fi

  if [ ! -f $OCAMLROOT/STAMP ] || [ "$OCAML_PORT:$(git rev-parse HEAD)" != "$(cat $OCAMLROOT/STAMP)" ]; then
    if [ ! -f $OCAMLROOT/STAMP ] ; then
      echo "$OCAMLROOT/STAMP missing"
    else
      echo "$OCAMLROOT/STAMP contains $(cat $OCAMLROOT/STAMP) where $OCAML_PORT:$(git rev-parse HEAD) expected"
    fi
    echo "Rebuilding OCaml $OCAMLBRANCH for $OCAML_PORT"
    configure_ocaml

    if [ "${OCAML_PORT/64/}" = "msvc" ] ; then
      eval $(tools/msvs-promote-path)
    fi

    if ! script -qec "$MAKEOCAML $FLEXDLL_BOOTSTRAP world opt" build-$OCAMLBRANCH-$OCAML_PORT.log >/dev/null 2>&1 ; then
      echo "Failed - uploading log"
      appveyor PushArtifact build-$OCAMLBRANCH-$OCAML_PORT.log
    elif ! script -qec "$MAKEOCAML install" install-$OCAMLBRANCH-$OCAML_PORT.log >/dev/null 2>&1 ; then
      echo "Failed to install - uploading logs"
      appveyor PushArtifact build-$OCAMLBRANCH-$OCAML_PORT.log
      appveyor PushArtifact install-$OCAMLBRANCH-$OCAML_PORT.log
    else
      echo "$OCAML_PORT:$(git rev-parse HEAD)" | tee $OCAMLROOT/STAMP
    fi
  fi
}

CHAINS="mingw mingw64 cygwin cygwin64 msvc msvc64"

cd $APPVEYOR_BUILD_FOLDER

case $1 in
  install)
    git tag merge

    # Do not perform end-of-line conversion
    git config --global core.autocrlf false
    if ((CYGWIN_BUILD)) ; then
      git clone https://github.com/ocaml/ocaml.git --branch $OCAMLBRANCH --depth 1 ocaml
    else
      git clone https://github.com/ocaml/ocaml.git --branch $OCAMLBRANCH --depth 1 --recurse-submodules ocaml
    fi

    cd ocaml
    check_ocaml
  ;;
  build)
    if [ -z "$2" ] ; then
      script -qec '"$0" $1 script' build | sed -e 's/\d027\[K//g' \
                                               -e 's/\d027\[m/\d027[0m/g' \
                                               -e 's/\d027\[01\([m;]\)/\d027[1\1/g'
      exit $?
    fi

    if [ "${OCAML_PORT/64/}" = "msvc" ] ; then
      eval $(ocaml/tools/msvs-promote-path)
    fi

    run "make flexlink.exe" make MSVC_DETECT=0 flexlink.exe

    for CHAIN in $CHAINS; do
      run "make build_$CHAIN" make build_$CHAIN
    done
  ;;
  test)
    if [ -z "$2" ] ; then
      script -qec '"$0" $1 script' test | sed -e 's/\d027\[K//g' \
                                              -e 's/\d027\[m/\d027[0m/g' \
                                              -e 's/\d027\[01\([m;]\)/\d027[1\1/g'
      exit $?
    fi

    if [ "${OCAML_PORT/64/}" = "msvc" ] ; then
      eval $(ocaml/tools/msvs-promote-path)
    fi

    for CHAIN in $CHAINS; do
      run "make demo_$CHAIN" make demo_$CHAIN
    done

    if [ "$SKIP_OCAML_TEST" != no ] ; then
      exit 0
    fi

    if [ -f ocamlopt.opt ] ; then
      git clean -dfx > /dev/null
      if [ -f flexdll/Compat.cmi ] ; then
        cd flexdll
        git clean -dfx > /dev/null
        cd ..
      fi
    fi

    configure_ocaml full

    # This tortures the ocamldoc compilation as it means that dllcamlstr,
    # dllunix and ocamlrun will all be more than 2GiB apart.
    sed -i -e "s/^LDOPTS=.*/\0 -ldopt -base -ldopt 0x210000000/" otherlibs/win32unix/Makefile
    sed -i -e "/^include \.\.\/Makefile/aLDOPTS=-ldopt -base -ldopt 0x310000000" otherlibs/str/Makefile

    cd flexdll
    git remote add local $(echo "$APPVEYOR_BUILD_FOLDER"| cygpath -f -) -f --tags
    run "git checkout $APPVEYOR_REPO_COMMIT" git checkout merge
    cd ..

    mv $OCAMLROOT $OCAMLROOT-Disabled
    run "make world" $MAKEOCAML $FLEXDLL_BOOTSTRAP world.opt
    run "make tests" $MAKEOCAML -C testsuite all
    mv $OCAMLROOT-Disabled $OCAMLROOT
  ;;
  *)
  echo "Unrecognised command: $1">&2
  exit 2
esac
