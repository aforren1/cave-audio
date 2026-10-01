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
#                 bumped placement's cells dropped, the first placement's kept. Twice: once with no
#                 reference arm, once with the knock landing inside the physical reference arm.
#   track_twist   the stand TWISTED 2 deg about the array center during the second placement (no center
#                 moves): the session reads directions, so it stops on the turn, exit 4, with the twisted
#                 placement's cells dropped and the first placement's kept.
#   ref_capture   the physical reference arm goes through the capture path: the counted captures, an
#                 injected capsule fault reaching the reference cells, the exclusion holding them.
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
  # no leading-zero strip: REGEX REPLACE re-anchors "^" after each match ("0505" -> "55"), and
  # math(EXPR) already reads "0505" as decimal 505
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

  # The same knock, landing INSIDE the physical reference arm. With the arm on, placement 1 is
  # 26 references + 26 x 3 x 2 matched phantoms + 54 grid cells = 236 captures, so the 241st is the 5th
  # capture of placement 2, a reference capture. A reference arm that skipped the capture path would skip
  # its bump checks too, and the knock would land 26 captures later, in the matched arm.
  math(EXPR per "26 + 26 * 3 * 2 + 9 * 3 * 2")
  math(EXPR knock "${per} + 5")
  run(bumpref 4 ${SIM} ${POS} --track-sim-bump ${knock} --out bumpref.csv)
  expect_err(bumpref "validate: BUMP: the ZM-1 moved 1[4-6]\\.[0-9] mm during placement 2, after capture 5 \\(limit 10\\.0 mm"
             "a knock during the reference arm is caught on the reference capture it lands on")
  expect(bumpref "placement 1 measured at [^\n]*\n +${per} bump check\\(s\\)"
         "every capture of placement 1, the 26 references included, was bump-checked")
  expect(bumpref "placement 2/2: [1-9][0-9]* cell\\(s\\) DROPPED \\(bumped\\)" "the bumped placement's reference cells are dropped")
  file(STRINGS "${WORK}/bumpref.csv" rows)
  list(REMOVE_AT rows 0)
  set(n0 0)
  set(nref 0)
  foreach(r IN LISTS rows)
    string(REPLACE "," ";" f "${r}")
    list(GET f 11 ref)
    list(GET f 13 lis)
    if(NOT lis EQUAL 0)
      message(FATAL_ERROR "validate_cli_test: bumpref: a cell from placement ${lis} survived the bump")
    endif()
    if(ref EQUAL 1)
      math(EXPR nref "${nref} + 1")
    endif()
    math(EXPR n0 "${n0} + 1")
  endforeach()
  if(NOT n0 EQUAL per OR NOT nref EQUAL 26)
    message(FATAL_ERROR "validate_cli_test: bumpref: placement 1 kept ${n0} cells (${nref} references), expected ${per} (26)")
  endif()
  message(STATUS "bumpref: the knock caught on reference capture 5 of placement 2; placement 1's ${n0} cells kept")

elseif(MODE STREQUAL "track_twist")
  # 54 captures per placement with no reference arm, as in track_bump: the twist lands on the 6th capture
  # of the SECOND placement. The center does not move, so only the orientation check can stop this.
  run(twist 4 ${SIM} ${POS} --no-reference --track-sim-twist 60 --out twist.csv)
  # both placements were PLACED: a stop that came from a gate that never opened is not this verdict
  grab_all(twist "placement: measured center" meas)
  list(LENGTH meas nm)
  if(NOT nm EQUAL 2)
    message(FATAL_ERROR "validate_cli_test: twist: ${nm} placements were taken, expected 2 (log: ${WORK}/twist.log)")
  endif()
  expect(twist "placement: position 2: target \\([^)]*\\), tolerance 20\\.0 mm, still within 2\\.0 mm and turning under 0\\.30 deg"
         "the gate has the orientation term")
  if(NOT "${twist_ERR}" MATCHES "validate: BUMP: the ZM-1 turned ([0-9]+\\.[0-9]+) deg during placement 2, after capture 6 \\(limit 0\\.41 deg\\),\n +its center ([0-9]+\\.[0-9]) mm from where it was taken")
    message(FATAL_ERROR "validate_cli_test: twist: no turn message naming the turn, the placement, the capture and the 0.41 deg limit (log: ${WORK}/twist.log)")
  endif()
  set(tdeg "${CMAKE_MATCH_1}")
  set(cmm "${CMAKE_MATCH_2}")
  string(REPLACE "." "" tm "${tdeg}")
  string(REGEX REPLACE "^0+([0-9])" "\\1" tm "${tm}")
  string(REPLACE "." "" cm "${cmm}")
  string(REGEX REPLACE "^0+([0-9])" "\\1" cm "${cm}")
  message(STATUS "twist: stopped on a turn of ${tdeg} deg (the script's 2.0), the center ${cmm} mm from the take")
  if(tm LESS 190 OR tm GREATER 210)
    message(FATAL_ERROR "validate_cli_test: twist: the turn read ${tdeg} deg, not the script's 2 deg")
  endif()
  if(cm GREATER 10)
    message(FATAL_ERROR "validate_cli_test: twist: the center moved ${cmm} mm: the twist was not about the center")
  endif()
  expect(twist "placement 1 measured at [^\n]*\n +54 bump check\\(s\\), the largest move 0\\.[0-9] mm \\(limit 10\\.0 mm\\), the largest turn 0\\.0[0-9] deg \\(limit 0\\.41 deg\\)"
         "placement 1 was turn-checked on every capture and never turned")
  expect(twist "placement 2/2: [1-9][0-9]* cell\\(s\\) DROPPED \\(bumped\\)" "the twisted placement's cells are dropped")
  if("${twist_OUT}" MATCHES "PASSED")
    message(FATAL_ERROR "validate_cli_test: twist: a stopped run reported a self-check PASSED")
  endif()
  csv_mics("${WORK}/twist.csv" rows)
  set(n0 0)
  foreach(row IN LISTS rows)
    string(REPLACE " " ";" f "${row}")
    list(GET f 0 lis)
    if(NOT lis EQUAL 0)
      message(FATAL_ERROR "validate_cli_test: twist: a cell from placement ${lis} survived the twist")
    endif()
    math(EXPR n0 "${n0} + 1")
  endforeach()
  if(NOT n0 EQUAL 54)
    message(FATAL_ERROR "validate_cli_test: twist: ${n0} cells of placement 1 written, expected all 54")
  endif()
  message(STATUS "twist: exit 4, placement 2 dropped, placement 1's ${n0} cells kept")
  run(twistnosim 2 --simulate --track-sim-twist 3)

elseif(MODE STREQUAL "background")
  # The background check (valid.h): one capture with no stimulus per placement, every cell held against
  # it. The background is the SIMULATOR's (--sim-background adds it to every capture, the silent one
  # included), never the code under test's. The simulated stimulus sits near -17 dBFS at the capsules,
  # so a -60 dBFS floor leaves every cell 40 dB clear and a -30 dBFS one puts every cell under 20 dB.
  set(ONE --simulate --azimuths 3 --no-prompt --position 0.3,1.5,-0.2)
  run(quiet 0 ${ONE} --out quiet.csv)
  expect(quiet "background: silent" "no simulated background: the silent capture reads silent")
  expect(quiet "background check: silent background, 236 cell\\(s\\) clear of it" "every cell scored against it")
  expect(quiet "placement 1/1: 236 captures," "the background capture is not one of the placement's cells")
  run(floor 0 ${ONE} --sim-background -60)
  expect(floor "background: -60\\.0 dBFS" "the background capture reads the simulated floor")
  expect(floor "background check: 0 of 236 cell\\(s\\) under 20 dB" "a quiet floor flags nothing")
  run(loud 0 ${ONE} --sim-background -30 --out loud.csv)
  expect(loud "background check: 236 of 236 cell\\(s\\) under 20 dB over the background \\(the lowest [0-9.]+ dB\\)   <-- the room is too loud"
         "a loud floor flags every cell, per placement")
  expect(loud "background check: 236 cell\\(s\\) under 20 dB over their placement's background" "and in the summary")
  file(STRINGS "${WORK}/loud.csv" rows)
  list(GET rows 0 hdr)
  if(NOT hdr MATCHES ",bg_db,bg_low$")
    message(FATAL_ERROR "validate_cli_test: background: the CSV carries no bg_db,bg_low columns")
  endif()
  list(GET rows 1 r1)
  if(NOT r1 MATCHES ",1$")
    message(FATAL_ERROR "validate_cli_test: background: the first loud row is not flagged bg_low (${r1})")
  endif()
  run(bgnosim 2 --sim-background -30)
  message(STATUS "background: silent, -60 dBFS (nothing flagged) and -30 dBFS (every cell flagged)")

elseif(MODE STREQUAL "ref_capture")
  # The physical reference arm is CAPTURED, through the same capture function as the phantoms, never
  # computed offline. On the rig the difference is the whole point (a floor measured in the real room
  # through the real instrument), but in simulate both would land near 0.1 deg, so the numbers cannot
  # tell them apart. Three things can:
  #   1. the capture count, COUNTED by the session and checked against the arithmetic here;
  #   2. --inject-fault, which reaches the capture path only: an offline reference cell is bit-identical
  #      with and without it, a captured one is not;
  #   3. the capsule exclusion reaching the reference scoring: with the faulty capsule excluded, every
  #      reference cell still lands under 1 deg.
  set(ONE --simulate --azimuths 3 --no-prompt --position 0.3,1.5,-0.2)
  run(clean 0 ${ONE} --out clean.csv)
  run(fault 0 ${ONE} --inject-fault 7 --out fault.csv)
  # the default grid's 26 speakers, 3 conditions, 9 directions
  math(EXPR per "26 + 26 * 3 * 2 + 9 * 3 * 2")
  expect(clean "placement 1/1: ${per} captures," "every cell, the 26 physical references included, is a capture")
  expect(clean "cost: ${per} captures per placement \\(26 of them physical references\\)" "the plan prices the reference arm")
  expect(fault "capsule check: 1 FAULTY  -  ch7" "the injected capsule is caught")
  function(ref_rows file out)
    file(STRINGS "${file}" rows)
    list(REMOVE_AT rows 0)
    set(res "")
    foreach(r IN LISTS rows)
      string(REPLACE "," ";" f "${r}")
      list(GET f 11 ref)
      if(ref EQUAL 1)
        list(APPEND res "${r}")
      endif()
    endforeach()
    set(${out} "${res}" PARENT_SCOPE)
  endfunction()
  ref_rows("${WORK}/clean.csv" rc)
  ref_rows("${WORK}/fault.csv" rf)
  list(LENGTH rc nc)
  list(LENGTH rf nf)
  if(NOT nc EQUAL 26 OR NOT nf EQUAL 26)
    message(FATAL_ERROR "validate_cli_test: ref_capture: ${nc} / ${nf} reference rows, expected 26 each")
  endif()
  if("${rc}" STREQUAL "${rf}")
    message(FATAL_ERROR "validate_cli_test: ref_capture: the reference cells are identical with and without "
                        "--inject-fault, so the fault never reached them: the physical reference arm is not "
                        "going through the capture path")
  endif()
  foreach(r IN LISTS rf)
    string(REPLACE "," ";" f "${r}")
    list(GET f 15 sp)
    list(GET f 25 miss)
    list(GET f 27 ok)
    if(NOT ok EQUAL 1 OR NOT miss MATCHES "^0\\.[0-9]+$")
      message(FATAL_ERROR "validate_cli_test: ref_capture: speaker ${sp} driven alone, with capsule 7 faulty, "
                          "missed by ${miss} deg (ok ${ok}): the capsule exclusion did not reach the reference scoring")
    endif()
  endforeach()
  message(STATUS "ref_capture: ${per} captures, the fault reached all 26 reference cells and the exclusion held them under 1 deg")

else()
  message(FATAL_ERROR "validate_cli_test: unknown MODE '${MODE}'")
endif()
