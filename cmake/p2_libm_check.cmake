# Build check for the vendored libm (docs/online/M5_1B_BUILD.md), run with cmake -P after the
# objcopy step that routes the game's math to p2libm (CMakeLists.txt).
#   NM          the nm program
#   PREFIX      the C symbol prefix ("" or "_" on Apple)
#   NAMES       cmake/p2_libm_names.txt
#   LIBS        "|"-separated static libraries whose undefined symbols are checked
#   NODEF_LIBS  "|"-separated libraries that must not define a libm name or a p2m_ name
#   STAMP       touched when everything passes
string(REPLACE "|" ";" LIBS "${LIBS}")
string(REPLACE "|" ";" NODEF_LIBS "${NODEF_LIBS}")

file(STRINGS "${NAMES}" _lines)
set(_banned)
set(_allowed)
foreach (_l IN LISTS _lines)
    if (_l MATCHES "^!([A-Za-z0-9_]+)$")
        list(APPEND _allowed "${CMAKE_MATCH_1}")
    elseif (_l MATCHES "^([A-Za-z0-9_]+)$")
        list(APPEND _banned "${CMAKE_MATCH_1}")
    endif ()
endforeach ()
list(REMOVE_ITEM _banned ${_allowed})

# Plain lower-case C symbol names of one library: undefined ones (nm type U or w) in _undef, defined
# ones in _def. Plain `nm` (no -u) prints the type letter on GNU, LLVM and Apple nm alike. Only such
# names can be libm functions or p2m_ ones; C++ names start with _Z.
function(_symbols lib)
    execute_process(COMMAND "${NM}" "${lib}" OUTPUT_VARIABLE _text RESULT_VARIABLE _rc ERROR_QUIET)
    if (NOT _rc EQUAL 0)
        message(FATAL_ERROR "p2libm check: ${NM} ${lib} failed (${_rc})")
    endif ()
    # One regex pass over the listing ("                 U sinf", "0000000000000000 T p2m_sinf").
    string(REGEX MATCHALL "[ \t][A-Za-z][ \t]+${PREFIX}[a-z][a-z0-9_]*\n" _hits "${_text}")
    set(_u)
    set(_d)
    foreach (_hit IN LISTS _hits)
        string(REGEX REPLACE "^[ \t]([A-Za-z])[ \t]+${PREFIX}([a-z0-9_]+)\n$" "\\1;\\2" _pair "${_hit}")
        list(GET _pair 0 _type)
        list(GET _pair 1 _name)
        if (_type STREQUAL "U" OR _type STREQUAL "w")
            list(APPEND _u "${_name}")
        else ()
            list(APPEND _d "${_name}")
        endif ()
    endforeach ()
    if (_u)
        list(REMOVE_DUPLICATES _u)
    endif ()
    if (_d)
        list(REMOVE_DUPLICATES _d)
    endif ()
    set(_undef "${_u}" PARENT_SCOPE)
    set(_def "${_d}" PARENT_SCOPE)
endfunction()

set(_errors)
foreach (_lib IN LISTS LIBS)
    _symbols("${_lib}")
    foreach (_s IN LISTS _undef)
        if (_s IN_LIST _banned)
            list(APPEND _errors "${_lib}: imports ${_s} from the system libm")
        endif ()
    endforeach ()
    if (_lib IN_LIST NODEF_LIBS)
        foreach (_s IN LISTS _def)
            if (_s IN_LIST _banned OR _s MATCHES "^p2m_")
                list(APPEND _errors "${_lib}: defines ${_s} (only p2libm may)")
            endif ()
        endforeach ()
    endif ()
endforeach ()

if (_errors)
    list(JOIN _errors "\n  " _msg)
    message(FATAL_ERROR "p2libm check failed:\n  ${_msg}\n"
            "Route the function in cmake/p2_new_syms.txt.in and port/libm/names.txt "
            "(and vendor it), or find out why the compiler emits it (sincos: -fno-builtin-sin/cos).")
endif ()
file(TOUCH "${STAMP}")
