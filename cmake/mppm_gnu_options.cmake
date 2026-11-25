# mppm_target_gnu_options(<target> <PRIVATE|PUBLIC|INTERFACE> <option>...)
#
# target_compile_options for options written in GCC/Clang style. With clang-cl (Windows) they are
# translated: "-include <file>" becomes /FI<file>, "-iquote <dir>" and every other option is passed
# to the Clang driver with /clang:, and -w becomes /w. Options inside $<$<COMPILE_LANGUAGE:X>:...>
# are translated too. clang-cl would otherwise drop the options it does not know with only a
# warning, and options like -ffp-contract=off matter for determinism.
#
# -fno-exceptions and -fno-rtti are left out for clang-cl: the MSVC STL expects exceptions and RTTI
# to match the rest of the program, and the game code throws nothing either way.
function(_mppm_cl_option opt out)
    if (opt STREQUAL "-w")
        set(${out} "/w" PARENT_SCOPE)
    elseif (opt STREQUAL "-fno-exceptions" OR opt STREQUAL "-fno-rtti")
        set(${out} "" PARENT_SCOPE)
    else ()
        set(${out} "/clang:${opt}" PARENT_SCOPE)
    endif ()
endfunction()

function(mppm_target_gnu_options target scope)
    if (NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target} ${scope} ${ARGN})
        return()
    endif ()
    set(result)
    set(pending "")
    foreach (opt IN LISTS ARGN)
        if (pending STREQUAL "-include")
            list(APPEND result "/FI${opt}")
            set(pending "")
        elseif (pending STREQUAL "-iquote")
            list(APPEND result "/clang:-iquote${opt}")
            set(pending "")
        elseif (opt STREQUAL "-include" OR opt STREQUAL "-iquote")
            set(pending "${opt}")
        elseif (opt MATCHES "^\\$<\\$<COMPILE_LANGUAGE:([A-Za-z,]+)>:(.*)>$")
            _mppm_cl_option("${CMAKE_MATCH_2}" inner)
            if (inner)
                list(APPEND result "$<$<COMPILE_LANGUAGE:${CMAKE_MATCH_1}>:${inner}>")
            endif ()
        else ()
            _mppm_cl_option("${opt}" one)
            if (one)
                list(APPEND result "${one}")
            endif ()
        endif ()
    endforeach ()
    if (pending)
        message(FATAL_ERROR "mppm_target_gnu_options(${target}): ${pending} without an argument")
    endif ()
    target_compile_options(${target} ${scope} ${result})
endfunction()
