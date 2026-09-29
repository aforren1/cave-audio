# Run bwa_speaker_survey and assert on its printed report. PASS_REGULAR_EXPRESSION cannot do this:
# it passes on ANY one match and ignores the exit code, so a run that printed one expected line and
# then crashed would pass. Here every EXPECT regex must match AND the exit code must be EXPECT_RC.
#   cmake -DTOOL=<exe> -DARGS=<a@@b@@...> -DEXPECT=<re1@@re2@@...> -DEXPECT_RC=<n> -P speaker_survey_check.cmake
# "@@" separates list items because the regexes carry the characters a CMake list would split on.
string(REPLACE "@@" ";" _args "${ARGS}")
string(REPLACE "@@" ";" _expect "${EXPECT}")
execute_process(COMMAND "${TOOL}" ${_args}
                OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
message("${_out}${_err}")
if(NOT "${_rc}" STREQUAL "${EXPECT_RC}")
  message(FATAL_ERROR "bwa_speaker_survey exited ${_rc}, expected ${EXPECT_RC}")
endif()
foreach(_re IN LISTS _expect)
  if(NOT _out MATCHES "${_re}")
    message(FATAL_ERROR "missing from the report: ${_re}")
  endif()
endforeach()
