# validate_cli_test.cmake - drives bwa_validate --track-sim through the tracked placement and checks the
# exit code, the printed report and the CSV (a ctest command line can check none of the numbers).
#
#   cmake -DEXE=<bwa_validate> -DMODE=<mode> -DWORK=<scratch dir> -P this
#
# MODE:
#   track_settle  two placements with --mount-offset x,y,z: each one WAITS (HOLD, then OK), takes the
#                 measured center, which is the scripted stand's TRUE center and not the plan, and every
#                 cell of that placement in the CSV carries it. Plus the flag refusals.
#   track_bump    the stand knocked 15 mm during the second placement: exit 4, the bump message, the
#                 bumped placement's cells dropped, the first placement's kept.
#
# The built-in default grid, no layout file. All scratch output goes under WORK, inside the build tree.

cmake_minimum_required(VERSION 3.20)
foreach(v EXE MODE WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "validate_cli_test: -D${v}= is required")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# two placements off the sweet spot, a small grid: the placement is under test, not the phantom
set(POS --position 0.3,1.5,-0.2 --position -0.5,1.2,0.4)
set(SIM --simulate --track-sim --azimuths 3)

function(run name want)
  execute_process(COMMAND "${EXE}" ${ARGN}
                  WORKING_DIRECTORY "${WORK}"
                  RESULT_VARIABLE rv OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r" "" out "${out}")
  string(REPLACE "\r" "" err "${err}")
  file(WRITE "${WORK}/${name}.log" "${out}\n--- stderr ---\n${err}")
  if(NOT rv EQUAL want)
    message(FATAL_ERROR "validate_cli_test: ${name}: exit ${rv}, expected ${want} (log: ${WORK}/${name}.log)\n${err}")
  endif()
  set(${name}_OUT "${out}" PARENT_SCOPE)
  set(${name}_ERR "${err}" PARENT_SCOPE)
endfunction()

function(expect name regex what)
  if(NOT "${${name}_OUT}" MATCHES "${regex}")
    message(FATAL_ERROR "validate_cli_test: ${name}: ${what} (no match for '${regex}'; log: ${WORK}/${name}.log)")
  endif()
endfunction()

function(expect_err name regex what)
  if(NOT "${${name}_ERR}" MATCHES "${regex}")
    message(FATAL_ERROR "validate_cli_test: ${name}: ${what} (no match for '${regex}' on stderr; log: ${WORK}/${name}.log)")
  endif()
endfunction()

# A decimal coordinate -> integer tenths of a millimeter (four decimals of a meter).
function(to_dmm out s)
  set(sign 1)
  if(s MATCHES "^[-+]")
    if(s MATCHES "^-")
      set(sign -1)
    endif()
    string(SUBSTRING "${s}" 1 -1 s)
  endif()
  if(NOT s MATCHES "^([0-9]+)\\.([0-9]*)$")
    message(FATAL_ERROR "validate_cli_test: cannot parse the coordinate '${s}'")
  endif()
  set(ip "${CMAKE_MATCH_1}")
  set(fp "${CMAKE_MATCH_2}0000")
  string(SUBSTRING "${fp}" 0 4 fp)
  string(REGEX REPLACE "^0+([0-9])" "\\1" fp "${fp}")
  math(EXPR v "${sign} * (${ip} * 10000 + ${fp})")
  set(${out} ${v} PARENT_SCOPE)
endfunction()

# every "<prefix> (x y z)" in the output, in order, as a list of "x;y;z"-free strings "x y z"
set(NUM "[-+]?[0-9]+\\.[0-9]+")
function(grab_all name prefix out)
  string(REGEX MATCHALL "${prefix} \\(${NUM} ${NUM} ${NUM}\\)" hits "${${name}_OUT}")
  set(pts "")
  foreach(h IN LISTS hits)
    string(REGEX REPLACE ".*\\((${NUM} ${NUM} ${NUM})\\)$" "\\1" p "${h}")
    list(APPEND pts "${p}")
  endforeach()
  set(${out} "${pts}" PARENT_SCOPE)
endfunction()

# squared distance between two "x y z" strings, in (tenths of a mm)^2
function(dist2 out a b)
  string(REPLACE " " ";" la "${a}")
  string(REPLACE " " ";" lb "${b}")
  set(d 0)
  foreach(k 0 1 2)
    list(GET la ${k} va)
    list(GET lb ${k} vb)
    to_dmm(ia "${va}")
    to_dmm(ib "${vb}")
    math(EXPR d "${d} + (${ia} - ${ib}) * (${ia} - ${ib})")
  endforeach()
  set(${out} ${d} PARENT_SCOPE)
endfunction()

# The CSV's rows as "lis mic_x mic_y mic_z" (columns 13 and 16 to 18; no field holds a comma).
function(csv_mics file out)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "validate_cli_test: ${file} was not written")
  endif()
  file(STRINGS "${file}" rows)
  list(REMOVE_AT rows 0)
  set(res "")
  foreach(r IN LISTS rows)
    string(REPLACE "," ";" f "${r}")
    list(GET f 13 lis)
    list(GET f 16 x)
    list(GET f 17 y)
    list(GET f 18 z)
    list(APPEND res "${lis} ${x} ${y} ${z}")
  endforeach()
  set(${out} "${res}" PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "track_settle")
  # A 40 mm tolerance: a center computed wrong by a few cm (the offset not rotated is 30 mm here) must
  # still pass the gate, so the check against the truth below is what catches it. At the default 20 mm
  # the gate would just time out, and the test would fail for the wrong reason.
  run(settle 0 ${SIM} ${POS} --mount-offset 0.03,-0.12,0.05 --place-tol-mm 40 --place-timeout 60
      --out settle.csv)
  expect(settle "track: mount offset \\(0\\.0300 -0\\.1200 0\\.0500\\) m in body axes, from --mount-offset"
         "the offset source is named")
  expect(settle "HOLD moving" "the readout shows the stand moving in: the gate WAITS")
  expect(settle "OK *\nplacement: measured center" "the gate opens and the placement is announced")
  expect(settle "track self-check PASSED" "the placement hook fired for both placements")
  grab_all(settle "placement: position [0-9]+: target" tgt)
  grab_all(settle "placement: measured center" meas)
  grab_all(settle "track-sim: true center" truth)
  list(LENGTH tgt nt)
  list(LENGTH meas nm)
  list(LENGTH truth nr)
  if(NOT nt EQUAL 2 OR NOT nm EQUAL 2 OR NOT nr EQUAL 2)
    message(FATAL_ERROR "validate_cli_test: settle: ${nt} targets, ${nm} measured, ${nr} true centers; expected 2 each")
  endif()
  csv_mics("${WORK}/settle.csv" rows)
  foreach(li 0 1)
    list(GET tgt ${li} t)
    list(GET meas ${li} m)
    list(GET truth ${li} r)
    dist2(d_mr "${m}" "${r}")
    dist2(d_mt "${m}" "${t}")
    message(STATUS "placement ${li}: plan (${t}), measured (${m}), true (${r})")
    # 1 mm from the truth (the window mean against one jittered instant is a tenth of that)
    if(d_mr GREATER 100)
      message(FATAL_ERROR "validate_cli_test: settle: placement ${li}: the measured center (${m}) is not the stand's true center (${r})")
    endif()
    # the script settles about 5 mm off the plan: the measured center must not be the plan
    if(d_mt LESS 900)
      message(FATAL_ERROR "validate_cli_test: settle: placement ${li}: the measured center (${m}) sits on the plan (${t})")
    endif()
    # every cell of the placement, reference cells included, is scored from the measured center
    set(ncell 0)
    foreach(row IN LISTS rows)
      string(REPLACE " " ";" f "${row}")
      list(GET f 0 lis)
      if(lis EQUAL li)
        list(GET f 1 x)
        list(GET f 2 y)
        list(GET f 3 z)
        if(NOT "${x} ${y} ${z}" STREQUAL "${m}")
          message(FATAL_ERROR "validate_cli_test: settle: placement ${li}: a cell was scored from (${x} ${y} ${z}), not the measured center (${m})")
        endif()
        math(EXPR ncell "${ncell} + 1")
      endif()
    endforeach()
    if(ncell EQUAL 0)
      message(FATAL_ERROR "validate_cli_test: settle: placement ${li} has no cells in the CSV")
    endif()
    message(STATUS "placement ${li}: ${ncell} cells, all at the measured center")
  endforeach()
  expect(settle "placement 2 measured at \\([^)]*\\), mount yaw -30\\.0 deg, tilt 1\\.5 deg;\n +[0-9]+ bump check\\(s\\), the largest move 0\\.[0-9] mm \\(limit 20\\.0 mm\\)"
         "the placement report carries the mount's yaw and a bump history with no bump")

  # the refusals: tracking flags without tracking, the simulated stand in front of a real array
  run(tolnotrack 2 --simulate --place-tol-mm 5)
  run(offnotrack 2 --simulate --mount-offset 0,0,0)
  run(bumpnosim 2 --simulate --track-sim-bump 3)
  run(simnosim 2 --track-sim)
  run(both 2 --simulate --track-sim --track 3)
  run(badtol 2 ${SIM} --place-tol-mm 0.5)
  # untracked --survey: a room-axes survey installs and a body-frame one is refused. It used to be
  # dropped silently, so a rig run fell back to the built-in table. The fixture is calibrate's, written
  # beside WORK in the build tree.
  get_filename_component(BIN "${WORK}" DIRECTORY)
  file(READ "${BIN}/calib_cli_track_survey_fixture.json" fx)
  file(WRITE "${WORK}/body.json" "${fx}")
  string(JSON fx REMOVE "${fx}" frame)
  string(JSON fx REMOVE "${fx}" mount_offset_m)
  file(WRITE "${WORK}/room.json" "${fx}")
  run(svbody 1 --simulate --azimuths 3 --position 0,1.5,0 --no-prompt --survey body.json)
  expect_err(svbody "is BODY-FRAME" "an untracked body-frame survey is refused")
  run(svroom 0 --simulate --azimuths 3 --position 0,1.5,0 --no-prompt --survey room.json)
  expect(svroom "capsule survey room.json installed" "an untracked room-axes survey is installed")

elseif(MODE STREQUAL "track_bump")
  # 54 captures per placement (3 conditions x 2 modes x 9 directions, no reference arm): the knock
  # lands on the 6th capture of the SECOND placement
  run(bump 4 ${SIM} ${POS} --no-reference --track-sim-bump 60 --out bump.csv)
  expect_err(bump "validate: BUMP: the ZM-1 moved 1[4-6]\\.[0-9] mm during placement 2, after capture 6 \\(limit 10\\.0 mm"
             "the bump message names the move, the placement and the capture")
  expect(bump "placement 2/2: [1-9][0-9]* cell\\(s\\) DROPPED \\(bumped\\)" "the bumped placement's cells are dropped")
  if("${bump_OUT}" MATCHES "PASSED")
    message(FATAL_ERROR "validate_cli_test: bump: a stopped run reported a self-check PASSED")
  endif()
  csv_mics("${WORK}/bump.csv" rows)
  set(n0 0)
  foreach(row IN LISTS rows)
    string(REPLACE " " ";" f "${row}")
    list(GET f 0 lis)
    if(NOT lis EQUAL 0)
      message(FATAL_ERROR "validate_cli_test: bump: a cell from placement ${lis} survived the bump")
    endif()
    math(EXPR n0 "${n0} + 1")
  endforeach()
  if(NOT n0 EQUAL 54)
    message(FATAL_ERROR "validate_cli_test: bump: ${n0} cells of placement 1 written, expected all 54")
  endif()
  message(STATUS "bump: exit 4, placement 2 dropped, placement 1's ${n0} cells kept")

else()
  message(FATAL_ERROR "validate_cli_test: unknown MODE '${MODE}'")
endif()
