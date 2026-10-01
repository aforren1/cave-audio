# calibrate_cli_test.cmake - drives bwa_calibrate through the multi-step flows a single ctest command
# line cannot express (trim, corrupt, verify), and checks exit codes plus the printed report.
#
#   cmake -DEXE=<bwa_calibrate> -DMODE=<mode> -DLAYOUT=<layout.json> -DWORK=<scratch dir> -P this
#
# MODE:
#   verify_omni   trims from a simulated run, then --verify: nothing flagged, exit 0. Then one speaker's
#                 delay_ms +0.5 ms and another's gain_db -3 dB: exactly those two flagged, exit 3.
#   verify_zylia  the same pair with the ZM-1 as the mic for both the trims and the verify pass.
#   zylia_trims   --zylia --trims end to end; its written trims must match the omni run's.
#   aim_sheet     --aim-sheet on the layout plus a listening point and one mis-aimed speaker.
#   live_zylia    --live N --zylia --simulate: a speaker moved 10 cm reads back as moved, and the move it
#                 is told is the opposite one in room words (room-right is -x), on all three axes; a
#                 layout with a plan_position is read against the PLAN, not the position; a speaker
#                 turned 25 deg off reads about 25 deg against a reference speaker and worse from the
#                 file behind a simulated screen; the tilt peaks where the box points at the mic.
#                 --ref-speakers: the latency from 4 (and 6) speakers' layout positions lands within 5 us of
#                 the simulator's own; one speaker 0.5 ms late (--sim-speaker-latency) is flagged alone and
#                 the median holds; two of three off is refused (exit 6); unmeasured positions are warned
#                 about; the --zylia position survey takes it too; and its refusals.
#   grid_rows     --room-eq-grid rows.txt on the simulated tracked stand: one placement per row, one grid
#                 written, keyed at each row's MEASURED center; 17 rows, a full grid, rows too close, and a
#                 bump mid-row are refused or stopped, and none of them writes a file.
#   track_settle  --zylia --trims --track-sim --mount-offset x,y,z: waits (HOLD, then OK), takes the measured
#                 center, which is the simulated stand's TRUE center and not the target, and solves there.
#   track_ring    the same with --mount-offset ring (the simulated MODELDEF's uneven marker ring).
#   track_bump    the stand knocked 15 mm mid-run: exit 4, the bump message, nothing written.
#   track_refuse  the refusals: the tracked --zylia survey without a body-frame survey (and with a room-axes
#                 one), the live position readout without one (the tilt meter still runs), stray flags.
#   track_survey  a body-frame survey (offset + table): the tracked position survey lands every speaker.
#   track_twist   the stand twisted 2 deg about the array center mid-run (no center moves). The --zylia
#                 survey and the live position readout read DIRECTIONS: exit 4 on a turn. A --zylia --trims
#                 run and the live tilt meter alone read only the center: the same twist runs to the end.
#   zylia_localize  --localize rows.txt --zylia on the turned stand, no survey: every speaker lands on the
#                 layout, because each range is the array center's arrival, which does not turn.
#   sweep_quality  the sweep-quality checks against a simulated interferer (--sim-interferer, its truth the
#                 simulator's): a trim run with a noise burst on three captures re-sweeps exactly those and
#                 writes the clean run's trims; a stray sweep louder than the speaker on every sweep of one
#                 speaker is flagged outside the window each time and the run writes nothing; live aiming
#                 with one contaminated reading does not hold it, and the peak is the clean one.
#   capsule_survey  --capsule-survey from the speakers: tracked with a wrong mount offset (the reloaded,
#                 re-aimed body-frame table matches the simulated physical array, the acoustic offset is the
#                 stand's TRUE one), tracked with the right one (no warning), untracked with a taped center
#                 that is off (a room-axes table, the acoustic center on the real one), and the refusals.
#                 Leave-one-out: the clean runs flag nothing; one speaker's layout position 80.6 mm off the
#                 simulator's truth is refused by name (exit 6), --drop-outliers drops it and lands on the
#                 clean table and center, and a second outlier behind it stops --drop-outliers (exit 6).
#                 The range check: one speaker 40 mm out RADIALLY, which leave-one-out holds out under its
#                 floor, is refused by its range (exit 6); a --latency 20 mm off flags nobody (the median
#                 takes it); with the latency poorly pinned (dilution over 3, the dome's upper side) it runs
#                 clean, and flags and drops a radial outlier.
#
# LAYOUT carries a directivity model (the calibrate_sim_room_* fixture). Every run uses the simulated
# room and an off-axis mic, so the directivity re-aim and the direct share are both in play. All
# scratch output goes under WORK, inside the build tree.

cmake_minimum_required(VERSION 3.20)
foreach(v EXE MODE LAYOUT WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "calibrate_cli_test: -D${v}= is required")
  endif()
endforeach()
get_filename_component(FIXDIR "${LAYOUT}" DIRECTORY)   # the build-tree fixtures sit beside the layout
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(MIC 0.6 1.2 0.4)
set(SIM --simulate --sim-room)

# run(<name> <expected exit> args...): runs bwa_calibrate in WORK, keeps its output in <name>_OUT and
# a log file, and fails the test on an unexpected exit code.
function(run name want)
  execute_process(COMMAND "${EXE}" ${ARGN}
                  WORKING_DIRECTORY "${WORK}"
                  RESULT_VARIABLE rv OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r" "" out "${out}")
  string(REPLACE "\r" "" err "${err}")
  file(WRITE "${WORK}/${name}.log" "${out}\n--- stderr ---\n${err}")
  if(NOT rv EQUAL want)
    message(FATAL_ERROR "calibrate_cli_test: ${name}: exit ${rv}, expected ${want} (log: ${WORK}/${name}.log)\n${err}")
  endif()
  set(${name}_OUT "${out}" PARENT_SCOPE)
endfunction()

function(expect name regex what)
  if(NOT "${${name}_OUT}" MATCHES "${regex}")
    message(FATAL_ERROR "calibrate_cli_test: ${name}: ${what} (no match for '${regex}'; log: ${WORK}/${name}.log)")
  endif()
endfunction()

# The converters below hand the fraction's digits to math(EXPR) as they are: it reads "0505" as 505.
# Do not strip the leading zeros with string(REGEX REPLACE "^0+..."): REGEX REPLACE re-anchors "^" after
# every match, so it turns "0505" into "55" and "0101" into "11", which misread 1.0505 m as 1.0055.
#
# A decimal JSON number -> integer thousandths (CMake math is integer only). Handles a sign, a
# fraction of any length (truncated to three digits) and a tiny value printed with a negative exponent.
function(to_milli out s)
  if(s MATCHES "[eE]-")
    set(${out} 0 PARENT_SCOPE)
    return()
  endif()
  set(sign 1)
  if(s MATCHES "^-")
    set(sign -1)
    string(SUBSTRING "${s}" 1 -1 s)
  endif()
  if(s MATCHES "^([0-9]+)\\.([0-9]*)$")
    set(ip "${CMAKE_MATCH_1}")
    set(fp "${CMAKE_MATCH_2}000")
    string(SUBSTRING "${fp}" 0 3 fp)
  elseif(s MATCHES "^([0-9]+)$")
    set(ip "${CMAKE_MATCH_1}")
    set(fp "000")
  else()
    message(FATAL_ERROR "calibrate_cli_test: cannot parse the number '${s}'")
  endif()
  math(EXPR v "${sign} * (${ip} * 1000 + ${fp})")
  set(${out} ${v} PARENT_SCOPE)
endfunction()

# A decimal number -> integer tenths of a millimeter (four decimals of a meter), for the --track modes.
function(to_dmm out s)
  set(sign 1)
  if(s MATCHES "^[-+]")
    if(s MATCHES "^-")
      set(sign -1)
    endif()
    string(SUBSTRING "${s}" 1 -1 s)
  endif()
  if(NOT s MATCHES "^([0-9]+)\\.([0-9]*)$")
    message(FATAL_ERROR "calibrate_cli_test: cannot parse the coordinate '${s}'")
  endif()
  set(ip "${CMAKE_MATCH_1}")
  set(fp "${CMAKE_MATCH_2}0000")
  string(SUBSTRING "${fp}" 0 4 fp)
  math(EXPR v "${sign} * (${ip} * 10000 + ${fp})")
  set(${out} ${v} PARENT_SCOPE)
endfunction()

# grab(<name> <regex with three captured coordinates> <var prefix>): x/y/z in tenths of a mm
function(grab name regex pfx)
  if(NOT "${${name}_OUT}" MATCHES "${regex}")
    message(FATAL_ERROR "calibrate_cli_test: ${name}: no match for '${regex}' (log: ${WORK}/${name}.log)")
  endif()
  set(a "${CMAKE_MATCH_1}")
  set(b "${CMAKE_MATCH_2}")
  set(c "${CMAKE_MATCH_3}")
  to_dmm(x "${a}")
  to_dmm(y "${b}")
  to_dmm(z "${c}")
  set(${pfx}_x ${x} PARENT_SCOPE)
  set(${pfx}_y ${y} PARENT_SCOPE)
  set(${pfx}_z ${z} PARENT_SCOPE)
  set(${pfx}_s "(${a} ${b} ${c})" PARENT_SCOPE)
endfunction()

# dist2(<out> <a> <b>): squared distance between two grabbed points, in (tenths of a mm)^2
function(dist2 out a b)
  math(EXPR dx "${${a}_x} - ${${b}_x}")
  math(EXPR dy "${${a}_y} - ${${b}_y}")
  math(EXPR dz "${${a}_z} - ${${b}_z}")
  math(EXPR d "${dx} * ${dx} + ${dy} * ${dy} + ${dz} * ${dz}")
  set(${out} ${d} PARENT_SCOPE)
endfunction()

set(NUM "([-+]?[0-9]+\\.[0-9]+)")

# A decimal number with up to six decimals -> integer micrometers (six decimals of a meter), for the
# capsule tables. No exponent: the tool prints these with %+.6f.
function(to_um out s)
  set(sign 1)
  if(s MATCHES "^[-+]")
    if(s MATCHES "^-")
      set(sign -1)
    endif()
    string(SUBSTRING "${s}" 1 -1 s)
  endif()
  if(NOT s MATCHES "^([0-9]+)\\.([0-9]*)$")
    message(FATAL_ERROR "calibrate_cli_test: cannot parse '${s}' as a plain decimal")
  endif()
  set(ip "${CMAKE_MATCH_1}")
  set(fp "${CMAKE_MATCH_2}000000")
  string(SUBSTRING "${fp}" 0 6 fp)
  math(EXPR v "${sign} * (${ip} * 1000000 + ${fp})")
  set(${out} ${v} PARENT_SCOPE)
endfunction()

# grab_caps(<name> <line tag> <var prefix>): the 19 "<tag> i (x y z)" lines of a run's output as three
# lists of micrometers, <pfx>_x/_y/_z, in capsule order
function(grab_caps name tag pfx)
  set(xs "")
  set(ys "")
  set(zs "")
  foreach(i RANGE 18)
    if(i LESS 10)
      set(ii " ${i}")
    else()
      set(ii "${i}")
    endif()
    if(NOT "${${name}_OUT}" MATCHES "\n${tag} ${ii} \\(${NUM} ${NUM} ${NUM}\\)")
      message(FATAL_ERROR "calibrate_cli_test: ${name}: no '${tag} ${ii}' line (log: ${WORK}/${name}.log)")
    endif()
    set(a "${CMAKE_MATCH_1}")
    set(b "${CMAKE_MATCH_2}")
    set(c "${CMAKE_MATCH_3}")
    to_um(x "${a}")
    to_um(y "${b}")
    to_um(z "${c}")
    list(APPEND xs ${x})
    list(APPEND ys ${y})
    list(APPEND zs ${z})
  endforeach()
  set(${pfx}_x "${xs}" PARENT_SCOPE)
  set(${pfx}_y "${ys}" PARENT_SCOPE)
  set(${pfx}_z "${zs}" PARENT_SCOPE)
endfunction()

# caps_worst(<out> <a> <b>): the worst capsule's squared distance between two grabbed tables, in um^2,
# and <out>_i its index
function(caps_worst out a b)
  set(worst 0)
  set(wi 0)
  foreach(i RANGE 18)
    foreach(ax x y z)
      list(GET ${a}_${ax} ${i} p)
      list(GET ${b}_${ax} ${i} q)
      math(EXPR d_${ax} "${p} - ${q}")
    endforeach()
    math(EXPR d "${d_x} * ${d_x} + ${d_y} * ${d_y} + ${d_z} * ${d_z}")
    if(d GREATER worst)
      set(worst ${d})
      set(wi ${i})
    endif()
  endforeach()
  set(${out} ${worst} PARENT_SCOPE)
  set(${out}_i ${wi} PARENT_SCOPE)
endfunction()

# A settled tracked trim run: it must WAIT (the readout shows HOLD before OK), take the measured
# center, and solve the trims there. The simulated captures come from the scripted stand's TRUE
# center, which mic_track computes on its own, so the solve's mic is checked against a truth the tool
# did not produce, and against the target, which it must NOT be.
function(track_settled name)
  # a 40 mm tolerance: a center computed wrong by a few cm must still pass the gate, so the check
  # against the truth below is what catches it (at 10 mm the gate would just time out)
  run(${name} 0 --layout "${LAYOUT}" --out ${name}.json ${SIM} --zylia --trims --track-sim
      --place-tol-mm 40 --place-timeout 60 ${ARGN})
  set(${name}_OUT "${${name}_OUT}" PARENT_SCOPE)      # the caller's expects read it too
  expect(${name} "HOLD moving" "the readout shows the stand moving in")
  expect(${name} "OK *\nplacement: measured center" "the gate opens and the placement is announced")
  grab(${name} "placement: [a-z-]+: target \\(${NUM} ${NUM} ${NUM}\\)" tgt)
  grab(${name} "placement: measured center \\(${NUM} ${NUM} ${NUM}\\), [0-9.]+ mm from the target; mount yaw -30\\.0 deg" meas)
  grab(${name} "track-sim: true center \\(${NUM} ${NUM} ${NUM}\\)" truth)
  grab(${name} "trims: arrivals aligned at the mic \\(${NUM} ${NUM} ${NUM}\\), the measured center" used)
  dist2(d_used_meas used meas)
  dist2(d_used_truth used truth)
  dist2(d_used_tgt used tgt)
  message(STATUS "${name}: target ${tgt_s}, measured ${meas_s}, true ${truth_s}, solved at ${used_s}")
  if(NOT d_used_meas EQUAL 0)
    message(FATAL_ERROR "calibrate_cli_test: ${name}: the trims were solved at ${used_s}, not the measured center ${meas_s}")
  endif()
  # 1 mm from the truth (the window mean against one jittered instant is a tenth of that)
  if(d_used_truth GREATER 100)
    message(FATAL_ERROR "calibrate_cli_test: ${name}: the measured center ${used_s} is not the stand's true center ${truth_s}")
  endif()
  # the script settles about 5 mm off the target: the solve must not sit on the target
  if(d_used_tgt LESS 900)
    message(FATAL_ERROR "calibrate_cli_test: ${name}: the trims were solved at the target ${tgt_s}, not where the mic stood")
  endif()
  expect(${name} "placement: trims measured at the measured center \\([^)]*\\), mount yaw -30\\.0 deg, tilt 1\\.5 deg;\n +26 bump check\\(s\\), the largest move 0\\.[0-9] mm \\(limit 20\\.0 mm\\)"
         "the report carries the center, the mount's yaw, and a bump history with no bump")
  if(NOT EXISTS "${WORK}/${name}.json")
    message(FATAL_ERROR "calibrate_cli_test: ${name}: no trims were written")
  endif()
endfunction()

# The trims pair: write trims, verify them clean, corrupt two speakers, verify again.
# ZY is empty for the omni or "--zylia" for the ZM-1.
function(verify_pair ZY)
  run(trims 0 --layout "${LAYOUT}" --out trims.json ${SIM} --mic ${MIC} ${ZY} --trims)
  run(verify_clean 0 --layout trims.json ${SIM} --mic ${MIC} ${ZY} --verify)
  expect(verify_clean "verify: 0 speaker\\(s\\) flagged" "the fresh trims must verify clean")
  expect(verify_clean "verify: arrival spread [0-9.]+ us" "the summary line is printed")

  # corrupt: the first speaker with a zero delay trim gets +0.5 ms, and the first OTHER speaker with a
  # zero gain trim (the reference, which the solve never cuts) gets -3 dB
  file(READ "${WORK}/trims.json" js)
  string(JSON n LENGTH "${js}" speakers)
  math(EXPR last "${n} - 1")
  set(ds -1)
  set(gs -1)
  foreach(k RANGE ${last})
    string(JSON d GET "${js}" speakers ${k} delay_ms)
    string(JSON g GET "${js}" speakers ${k} gain_db)
    to_milli(dm "${d}")
    to_milli(gm "${g}")
    if(ds EQUAL -1 AND dm EQUAL 0)
      set(ds ${k})
    elseif(gs EQUAL -1 AND gm EQUAL 0)
      set(gs ${k})
    endif()
  endforeach()
  if(ds EQUAL -1 OR gs EQUAL -1)
    message(FATAL_ERROR "calibrate_cli_test: no zero-delay and zero-gain speaker pair to corrupt")
  endif()
  string(JSON js SET "${js}" speakers ${ds} delay_ms 0.5)
  string(JSON js SET "${js}" speakers ${gs} gain_db -3)
  file(WRITE "${WORK}/corrupt.json" "${js}")

  run(verify_bad 3 --layout corrupt.json ${SIM} --mic ${MIC} ${ZY} --verify)
  expect(verify_bad "verify: 2 speaker\\(s\\) flagged" "exactly two speakers must be flagged")
  expect(verify_bad "\n +${ds} +\\+[0-9.]+ +[-+][0-9.]+   <- ARRIVAL\n" "speaker ${ds} (delay +0.5 ms) must flag ARRIVAL only")
  expect(verify_bad "\n +${gs} +[-+][0-9.]+ +-[0-9.]+   <- LEVEL\n" "speaker ${gs} (gain -3 dB) must flag LEVEL only")
  string(REGEX MATCHALL "<- (ARRIVAL|LEVEL|DEAD)" hits "${verify_bad_OUT}")
  list(LENGTH hits nh)
  if(NOT nh EQUAL 2)
    message(FATAL_ERROR "calibrate_cli_test: verify_bad: ${nh} flags printed, expected 2")
  endif()
  message(STATUS "verify pair${ZY}: clean, then speakers ${ds} (delay) and ${gs} (gain) flagged")
endfunction()

if(MODE STREQUAL "verify_omni")
  verify_pair("")

elseif(MODE STREQUAL "verify_zylia")
  verify_pair("--zylia")

elseif(MODE STREQUAL "zylia_trims")
  run(omni 0 --layout "${LAYOUT}" --out omni.json ${SIM} --mic ${MIC})
  run(zylia 0 --layout "${LAYOUT}" --out zylia.json ${SIM} --mic ${MIC} --zylia --trims)
  expect(zylia "power mean over its 19 capsules" "the ZM-1 trim run announces the proxy")
  file(READ "${WORK}/omni.json" jo)
  file(READ "${WORK}/zylia.json" jz)
  string(JSON n LENGTH "${jo}" speakers)
  math(EXPR last "${n} - 1")
  set(worst_g 0)
  set(worst_d 0)
  foreach(k RANGE ${last})
    foreach(f gain_db delay_ms)
      string(JSON a GET "${jo}" speakers ${k} ${f})
      string(JSON b GET "${jz}" speakers ${k} ${f})
      to_milli(am "${a}")
      to_milli(bm "${b}")
      math(EXPR dd "${am} - ${bm}")
      if(dd LESS 0)
        math(EXPR dd "-${dd}")
      endif()
      if(f STREQUAL "gain_db" AND dd GREATER worst_g)
        set(worst_g ${dd})
      elseif(f STREQUAL "delay_ms" AND dd GREATER worst_d)
        set(worst_d ${dd})
      endif()
    endforeach()
  endforeach()
  message(STATUS "zylia vs omni trims: worst gain difference ${worst_g} mdB, worst delay difference ${worst_d} us")
  # 0.25 dB: the power mean samples 19 points of the room's field, the omni one; 25 us: the trims are
  # whole samples (20.8 us), so the two can round one sample apart and no further
  if(worst_g GREATER 250 OR worst_d GREATER 25)
    message(FATAL_ERROR "calibrate_cli_test: the ZM-1 trims differ from the omni trims by ${worst_g} mdB / ${worst_d} us")
  endif()

elseif(MODE STREQUAL "aim_sheet")
  # the listening point the user plans (4.75 ft), speaker 4 (the floor center, directly below it)
  # aimed explicitly and correctly, speaker 5 (the floor, 1.5 m ahead) aimed explicitly straight up:
  # 46 degrees off the listening point
  file(READ "${LAYOUT}" js)
  string(JSON p4 GET "${js}" speakers 4 position)
  string(JSON p5 GET "${js}" speakers 5 position)
  string(REGEX REPLACE "[ \n\r\t]" "" p4 "${p4}")
  string(REGEX REPLACE "[ \n\r\t]" "" p5 "${p5}")
  if(NOT p4 MATCHES "^\\[0,0(\\.0)?,0\\]$" OR NOT p5 MATCHES "^\\[0,0(\\.0)?,1\\.5\\]$")
    message(FATAL_ERROR "calibrate_cli_test: the fixture's speakers 4/5 moved (${p4} ${p5}); update this test")
  endif()
  string(JSON js SET "${js}" listening_point_m "[0, 1.448, 0]")
  string(JSON js SET "${js}" speakers 4 aim "[0, 1, 0]")
  string(JSON js SET "${js}" speakers 5 aim "[0, 1, 0]")
  file(WRITE "${WORK}/aim_layout.json" "${js}")
  run(sheet 3 --layout aim_layout.json --aim-sheet aim.csv)
  expect(sheet "listening point: declared \\(listening_point_m\\) \\(0\\.000 1\\.448 0\\.000\\), height 1\\.448 m \\(4\\.75 ft\\)"
         "the declared listening point and its height in m and ft")
  expect(sheet "directivity model: present" "the model is reported")
  expect(sheet "explicit aims: 2 of 26" "two explicit aims")
  expect(sheet "spk  5: layout aim is 46\\.[0-9] deg off the listening point \\(explicit aim" "speaker 5 is flagged")
  expect(sheet "aim-sheet: 1 speaker\\(s\\) aimed more than 20 deg" "only speaker 5 is flagged")
  file(STRINGS "${WORK}/aim.csv" rows)
  list(LENGTH rows nr)
  if(NOT nr EQUAL 27)
    message(FATAL_ERROR "calibrate_cli_test: aim.csv has ${nr} rows, expected a header plus 26")
  endif()
  list(GET rows 0 hdr)
  if(NOT hdr STREQUAL "speaker,x_m,y_m,z_m,target_x_m,target_y_m,target_z_m,distance_m,aim_x,aim_y,aim_z,bearing_deg,down_tilt_deg,layout_aim_x,layout_aim_y,layout_aim_z,layout_aim_source,layout_bearing_deg,layout_down_tilt_deg,layout_aim_off_deg,loss_2k_db,loss_16k_db,flag")
    message(FATAL_ERROR "calibrate_cli_test: unexpected CSV header: ${hdr}")
  endif()
  set(flagged "")
  foreach(r IN LISTS rows)
    if(r MATCHES ",OFF_AIM$")
      string(REGEX MATCH "^[0-9]+" idx "${r}")
      list(APPEND flagged ${idx})
    endif()
  endforeach()
  if(NOT flagged STREQUAL "5")
    message(FATAL_ERROR "calibrate_cli_test: CSV flags speakers '${flagged}', expected only 5")
  endif()
  list(GET rows 5 r4)
  list(GET rows 6 r5)
  # speaker 4 sits under the listening point: aim straight up, down-tilt -90, explicit, on aim
  if(NOT r4 MATCHES "^4,.*,-90\\.0,0\\.0000,1\\.0000,0\\.0000,explicit,0\\.0,-90\\.0,0\\.0,-?0\\.0,-?0\\.0,$")
    message(FATAL_ERROR "calibrate_cli_test: speaker 4's row is wrong: ${r4}")
  endif()
  # speaker 5, 1.5 m ahead on the floor: it should face behind (bearing 180) and up (negative down-tilt)
  if(NOT r5 MATCHES "^5,.*,180\\.0,-4[0-9]\\.[0-9],0\\.0000,1\\.0000,0\\.0000,explicit,.*,OFF_AIM$")
    message(FATAL_ERROR "calibrate_cli_test: speaker 5's row is wrong: ${r5}")
  endif()

  # A failed trilateration leaves the speaker where the file had it. Five COLLINEAR mic rows make
  # every solve degenerate; the tool used to write (0,0,0), a confident position the engine would
  # then render from.
  file(WRITE "${WORK}/line.txt" "0 1.5 0\n0.3 1.5 0\n0.6 1.5 0\n0.9 1.5 0\n1.2 1.5 0\n")
  run(degen 0 --layout "${LAYOUT}" --out degen.json --simulate --localize line.txt)
  if(NOT "${degen_OUT}" MATCHES "failed ones left as they were")
    message(FATAL_ERROR "calibrate_cli_test: degen: the solves did not fail as planned (log: ${WORK}/degen.log)")
  endif()
  file(READ "${WORK}/degen.json" dj)
  string(JSON n LENGTH "${js}" speakers)
  math(EXPR last "${n} - 1")
  foreach(k RANGE ${last})
    string(JSON want GET "${js}" speakers ${k} position)
    string(JSON got  GET "${dj}" speakers ${k} position)
    string(REGEX REPLACE "[ \n\t]" "" want "${want}")
    string(REGEX REPLACE "[ \n\t]" "" got "${got}")
    foreach(a 0 1 2)
      string(JSON wa GET "${want}" ${a})
      string(JSON ga GET "${got}" ${a})
      to_milli(mw "${wa}")
      to_milli(mg "${ga}")
      if(NOT mw EQUAL mg)
        message(FATAL_ERROR "calibrate_cli_test: degen: speaker ${k} was moved from ${want} to ${got} by a failed solve")
      endif()
    endforeach()
  endforeach()

elseif(MODE STREQUAL "live_zylia")
  # the rig's placement: the ZM-1 at the 4.75 ft listening point, which every default aim points at
  file(READ "${LAYOUT}" js)
  string(JSON has_oa ERROR_VARIABLE jerr GET "${js}" directivity on_axis_db 0)
  if(jerr)
    message(FATAL_ERROR "calibrate_cli_test: the fixture's directivity carries no on_axis_db (regenerate it with clf_to_json.py)")
  endif()
  string(JSON js SET "${js}" listening_point_m "[0, 1.448, 0]")
  file(WRITE "${WORK}/live_layout.json" "${js}")
  set(LIVE --layout live_layout.json --live 7 --zylia --simulate --sim-room)

  # position: speaker 7 (1.5, 0, 0) is 10 cm further along +x than the layout says
  run(pos 0 ${LIVE} --sim-move 0.1 0 0 --sweeps 1)
  if(NOT pos_OUT MATCHES "#1 +pos +([-+][0-9.]+) +([-+][0-9.]+) +([-+][0-9.]+) mm \\(\\|d\\| +([0-9.]+)\\)  dir ([0-9.]+) deg")
    message(FATAL_ERROR "calibrate_cli_test: pos: no position line (log: ${WORK}/pos.log)")
  endif()
  set(px "${CMAKE_MATCH_1}")
  set(py "${CMAKE_MATCH_2}")
  set(pz "${CMAKE_MATCH_3}")
  set(pdir "${CMAKE_MATCH_5}")
  message(STATUS "live position: injected (+100 0 0) mm, read (${px} ${py} ${pz}) mm, direction error ${pdir} deg")
  string(REPLACE "+" "" px "${px}")
  string(REPLACE "+" "" py "${py}")
  string(REPLACE "+" "" pz "${pz}")
  to_milli(mx "${px}")
  to_milli(my "${py}")
  to_milli(mz "${pz}")
  # 5 mm: the capsule arrivals are cross-correlated (zylia_ir_tdoa), which reads the direction to a few
  # hundredths of a degree in simulation; each capsule's own |IR| peak read 0.5 deg (18 mm at 2.1 m)
  # and failed this bound at +88.7 -13.2 +3.2 mm
  if(mx LESS 95000 OR mx GREATER 105000 OR my LESS -5000 OR my GREATER 5000 OR mz LESS -5000 OR mz GREATER 5000)
    message(FATAL_ERROR "calibrate_cli_test: pos: the 10 cm move read back as (${px} ${py} ${pz}) mm")
  endif()
  expect(pos "live: speaker 7 against its layout position and aim \\(the file carries no plan\\)"
         "with no plan in the file, the header says the readout targets the layout")
  # The move in installer words, read off that same line. The box sits 10 cm toward +x, which is
  # room-LEFT (room-right is -x), so the instruction is "toward room-right". Spelled here by hand from
  # the frame, not from the tool's own constants.
  expect(pos "#1 +pos [^\n]*\\| move (9[5-9]|10[0-5]) mm toward room-right \\|"
         "a box 10 cm toward +x is told to move toward room-right")
  # All three axes at once: 20 mm toward -x (room-right), 30 mm up, 40 mm toward -z (the back wall); the
  # instruction is each one undone, in the order right/left, up/down, front/back.
  run(pos3 0 ${LIVE} --sim-move -0.02 0.03 -0.04 --sweeps 1)
  expect(pos3 "#1 +pos [^\n]*\\| move (1[89]|2[0-2]) mm toward room-left, (2[89]|3[0-2]) mm down, (3[89]|4[0-2]) mm toward the front wall \\|"
         "a box off on all three axes gets the opposite move on each, in room words")

  # Live aiming reads the PLAN. Speaker 7's plan sits 60 mm toward -x of its layout position (the
  # as-built, where the simulated box is), and its planned aim points straight up. Read against the
  # plan, the box reads +60 mm and is told toward room-right; read against `position` it would read 0.
  string(JSON pj SET "${js}" speakers 7 plan_position "[1.44, 0, 0]")
  string(JSON pj SET "${pj}" speakers 7 plan_aim "[0, 1, 0]")
  file(WRITE "${WORK}/live_plan.json" "${pj}")
  run(plan 0 --layout live_plan.json --live 7 --zylia --simulate --sim-room --sweeps 1)
  expect(plan "live: speaker 7 against its PLAN \\(plan_position, plan_aim\\)[^\n]*target \\(1\\.440 0\\.000 0\\.000\\)"
         "the header names the plan as the target")
  expect(plan "live: as-built \\(the layout's position and aim\\) is 60\\.0 mm from the plan \\(move 60 mm toward room-right\\)"
         "the header gives the as-built against the plan, in room words")
  expect(plan "#1 +pos +\\+(5[5-9]|6[0-5])\\.[0-9] +[-+][0-4]\\.[0-9] +[-+][0-4]\\.[0-9] mm[^\n]*\\| move (5[5-9]|6[0-5]) mm toward room-right \\|"
         "the reading is the box against its PLAN, not against its layout position")
  # straight up from (1.44, 0, 0) against the line to the ZM-1 at (0, 1.448, 0): acos(1.448 / 2.042) = 44.8 deg
  expect(plan "\\| plan 44\\.[0-9] deg" "the angle the line compares against is the plan's aim")

  # aim: turned 25 deg off, then 12, then on axis, then back out. The reference is speaker 16, which
  # points at the mic; a 3 dB screen shelf (above 4 kHz) sits on every path, so the file's 0 deg tilt
  # is wrong by the screen's tilt and the reference's is not. 3 dB, not 1: the live bands (3-10 kHz
  # against 10 kHz up) both sit mostly above the shelf's corner, so a 1 dB shelf tilts them too
  # little (the file read 26.3 against the reference's 24.4) to test the point
  # The box is HELD on axis for two readings (0,0): the peak hold only takes a level two clean readings
  # in a row agree on (calib_peak_update), which is what an installer pausing at the peak gives it
  run(aim 0 ${LIVE} --sim-screen 3 --aim-ref 16 --sim-aim-steps 25,12,0,0,12,25)
  expect(aim "on-axis reference from speaker 16" "the reference is taken")
  if(NOT aim_OUT MATCHES "\n  #1 [^\n]*off-axis ref ([0-9.]+) deg \\[[0-9.]+-[0-9.]+\\], file ([0-9.]+) deg[^\n]*true 25\\.0 deg\n")
    message(FATAL_ERROR "calibrate_cli_test: aim: reading 1 has no ref and file estimate (log: ${WORK}/aim.log)")
  endif()
  set(aref "${CMAKE_MATCH_1}")
  set(afile "${CMAKE_MATCH_2}")
  to_milli(mref "${aref}")
  to_milli(mfile "${afile}")
  message(STATUS "live aim, true 25 deg, room + 3 dB screen: reference ${aref} deg, file ${afile} deg")
  if(mref LESS 22000 OR mref GREATER 28000)
    message(FATAL_ERROR "calibrate_cli_test: aim: 25 deg read ${aref} deg against the reference")
  endif()
  math(EXPR gap "${mfile} - ${mref}")
  if(gap LESS 3000)
    message(FATAL_ERROR "calibrate_cli_test: aim: the file read ${afile} deg, not clearly worse than the reference's ${aref}: the screen went missing")
  endif()
  expect(aim "\n  #3 [^\n]*peak -- [^\n]*true 0\\.0 deg\n" "one reading on axis sets no peak yet")
  expect(aim "\n  #4 [^\n]*below 0\\.00 dB[^\n]*off-axis ref on axis \\(under [0-9]+ deg\\)[^\n]*true 0\\.0 deg\n"
         "held on axis, the meter sits at its peak and the reference reads 'on axis'")
  expect(aim "\n  #5 [^\n]*below 0\\.[0-9][0-9] dB" "turned back out, the meter drops")
  expect(aim "live: peak tilt [-+][0-9.]+ dB at reading 4, where the true axis was 0\\.0 deg off the mic"
         "the peak is found where the box points at the mic")

  # anechoic and no screen: the file's 0 deg tilt is the simulated speaker's own, so it agrees
  run(aim_clean 0 --layout live_layout.json --live 7 --zylia --simulate --aim-ref 16 --sim-aim-steps 25)
  if(NOT aim_clean_OUT MATCHES "\n  #1 [^\n]*off-axis ref ([0-9.]+) deg \\[[0-9.]+-[0-9.]+\\], file ([0-9.]+) deg")
    message(FATAL_ERROR "calibrate_cli_test: aim_clean: no estimate (log: ${WORK}/aim_clean.log)")
  endif()
  set(cref "${CMAKE_MATCH_1}")
  set(cfile "${CMAKE_MATCH_2}")
  to_milli(mcref "${cref}")
  to_milli(mcfile "${cfile}")
  message(STATUS "live aim, true 25 deg, anechoic, no screen: reference ${cref} deg, file ${cfile} deg")
  if(mcref LESS 22000 OR mcref GREATER 28000 OR mcfile LESS 22000 OR mcfile GREATER 28000)
    message(FATAL_ERROR "calibrate_cli_test: aim_clean: 25 deg read ${cref} (ref) / ${cfile} (file)")
  endif()

  # the flags that make no sense together are refused
  run(two_refs 2 ${LIVE} --aim-ref 16 --aim-ref-db -0.4)
  run(sim_on_rig 2 --layout live_layout.json --live 7 --zylia --sim-move 0.1 0 0)
  run(omni_ref 2 --layout live_layout.json --live 7 --simulate --aim-ref 16)

  # --ref-speakers: the latency from speakers whose positions are trusted. The truth is the simulator's own
  # latency, 512 samples at 48 kHz = 10.6667 ms (CAL_SIM_LATENCY_SAMPLES), which the tool reads nowhere on
  # this path; in tenths of a microsecond, 106667. 5 us (1.7 mm at c) is the tolerance: the pooled center
  # arrival reads about 1 us long in simulation (its plane-wave bias), and a wrong distance or a wrong
  # speaker is tens of microseconds or more.
  set(LAT_TRUE 106667)
  function(lat_check name tag)
    if(NOT "${${name}_OUT}" MATCHES "\n${tag}: latency ([0-9]+)\\.([0-9][0-9][0-9][0-9]) ms \\([0-9.]+ m at c\\), the median of ([0-9]+)")
      message(FATAL_ERROR "calibrate_cli_test: ${name}: no --ref-speakers latency line (log: ${WORK}/${name}.log)")
    endif()
    math(EXPR got "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2}")
    math(EXPR d "${got} - ${LAT_TRUE}")
    if(d LESS 0)
      math(EXPR d "-${d}")
    endif()
    message(STATUS "${name}: --ref-speakers latency ${CMAKE_MATCH_1}.${CMAKE_MATCH_2} ms over ${CMAKE_MATCH_3} speakers, true 10.6667 ms: ${d} x 0.1 us off")
    if(d GREATER 50)
      message(FATAL_ERROR "calibrate_cli_test: ${name}: the latency is ${CMAKE_MATCH_1}.${CMAKE_MATCH_2} ms, more than 5 us off the true 10.6667 ms")
    endif()
  endfunction()

  # Four speakers, clean: the true latency, nothing flagged, and a warning per speaker, because this
  # fixture carries no plan_position, so nothing ever measured these positions
  run(refspk 0 ${LIVE} --sweeps 1 --ref-speakers 2,10,18,24)
  lat_check(refspk live)
  if(refspk_OUT MATCHES "FLAGGED")
    message(FATAL_ERROR "calibrate_cli_test: refspk: a clean --ref-speakers run flagged a speaker (log: ${WORK}/refspk.log)")
  endif()
  string(REGEX MATCHALL "WARNING --ref-speakers [0-9]+ carries no plan_position" np "${refspk_OUT}")
  list(LENGTH np nnp)
  if(NOT nnp EQUAL 4)
    message(FATAL_ERROR "calibrate_cli_test: refspk: ${nnp} unmeasured-position warnings for 4 speakers with no plan (log: ${WORK}/refspk.log)")
  endif()

  # The same four where a survey recorded a plan for 2, 10 and 18 (moved 30 mm from it), and speaker 24's
  # plan equals its position: only 24 is warned about
  string(JSON pj2 SET "${js}" speakers 2 plan_position "[-1.47, 0, 1.5]")
  string(JSON pj2 SET "${pj2}" speakers 10 plan_position "[-1.47, 1.5, 0]")
  string(JSON pj2 SET "${pj2}" speakers 18 plan_position "[-1.47, 3, 0]")
  string(JSON pj2 SET "${pj2}" speakers 24 plan_position "[1.5, 3, 0]")
  file(WRITE "${WORK}/ref_plan.json" "${pj2}")
  run(refplan 0 --layout ref_plan.json --live 7 --zylia --simulate --sim-room --sweeps 1 --ref-speakers 2,10,18,24)
  lat_check(refplan live)
  string(REGEX MATCHALL "WARNING --ref-speakers [0-9]+" pw "${refplan_OUT}")
  if(NOT pw STREQUAL "WARNING --ref-speakers 24" OR NOT refplan_OUT MATCHES "--ref-speakers 24: its plan_position equals its position")
    message(FATAL_ERROR "calibrate_cli_test: refplan: the warnings are '${pw}', not speaker 24's equal plan alone (log: ${WORK}/refplan.log)")
  endif()

  # Speaker 10 plays 0.5 ms late (--sim-speaker-latency, a Dante setting): six speakers, 10 flagged alone,
  # and the median still the true latency. Six, not four: a MEAN would sit 83 us late, under the flag, so
  # the latency check is what catches it
  run(refdante 0 ${LIVE} --sweeps 1 --ref-speakers 2,6,10,18,20,24 --sim-speaker-latency 10,0.5)
  lat_check(refdante live)
  string(REGEX MATCHALL "   FLAGGED" fl "${refdante_OUT}")
  list(LENGTH fl nfl)
  if(NOT nfl EQUAL 1 OR NOT refdante_OUT MATCHES "\n  spk 10: [^\n]*, +\\+(4[89][0-9]|5[01][0-9])\\.[0-9] us from the median   FLAGGED")
    message(FATAL_ERROR "calibrate_cli_test: refdante: speaker 10 is not flagged alone about 500 us late (log: ${WORK}/refdante.log)")
  endif()
  expect(refdante "WARNING speaker 10 arrives [0-9.]+ us later than its layout distance says[^\n]*\n[^\n]*Dante latency setting differs"
         "the flag names the Dante setting")

  # Two of three off: no majority, nothing used, exit 6
  run(refsplit 6 ${LIVE} --sweeps 1 --ref-speakers 2,10,18 --sim-speaker-latency 10,0.5 --sim-speaker-latency 18,-0.4)
  file(READ "${WORK}/refsplit.log" lg)
  if(NOT lg MATCHES "2 of the 3 speakers are more than 100 us off the median, so no majority")
    message(FATAL_ERROR "calibrate_cli_test: refsplit: two bad of three were not refused (log: ${WORK}/refsplit.log)")
  endif()

  # The --zylia position survey: the same latency from the same speakers (the exact-wavefront simulation
  # carries the simulator's own latency too), the offset flagged, and the positions written
  run(refzy 0 --layout live_layout.json --zylia --simulate --mic 0 1.448 0 --ref-speakers 2,6,10,18,20,24
      --sim-speaker-latency 10,0.5 --out refzy.json)
  lat_check(refzy zylia)
  if(NOT refzy_OUT MATCHES "\n  spk 10: [^\n]*us from the median   FLAGGED" OR NOT EXISTS "${WORK}/refzy.json")
    message(FATAL_ERROR "calibrate_cli_test: refzy: speaker 10 not flagged, or no positions written (log: ${WORK}/refzy.log)")
  endif()
  expect(refzy "\n  spk 18: dir=[^\n]*\\[--ref-speakers\\]" "the listed speakers are marked in the survey")

  # refusals, each for its own reason
  run(refboth 2 ${LIVE} --sweeps 1 --ref 2 2.5 --ref-speakers 2,10,18)
  run(reftwo 2 ${LIVE} --sweeps 1 --ref-speakers 2,10)
  run(reflive 2 ${LIVE} --sweeps 1 --ref-speakers 7,10,18)
  run(refnomic 2 --layout live_layout.json --zylia --simulate --ref-speakers 2,10,18)
  # --ref where nothing reads it used to be ignored silently
  run(reftrims 2 --layout live_layout.json --zylia --trims --simulate --ref 2 2.5)
  if(NOT "${reftrims_ERR}" MATCHES "--ref sets the latency for --live N --zylia and the --zylia position survey only")
    file(READ "${WORK}/reftrims.log" rtlog)
    if(NOT rtlog MATCHES "--ref sets the latency for")
      message(FATAL_ERROR "calibrate_cli_test: reftrims: --ref with --trims was not refused for its own reason (log: ${WORK}/reftrims.log)")
    endif()
  endif()
  run(reftrims 2 --layout live_layout.json --zylia --simulate --trims --ref-speakers 2,10,18)
  run(refcaps 2 --layout live_layout.json --zylia --simulate --mic 0 1.448 0 --capsule-survey rc.json --ref-speakers 2,10,18)
  run(simlatrig 2 --layout live_layout.json --live 7 --zylia --sim-speaker-latency 10,0.5)
  foreach(pair "refboth|--ref and --ref-speakers both set the latency" "reftwo|a median needs 3 or more"
               "reflive|the one live aiming moves" "refnomic|pass --mic x y z"
               "reftrims|--ref-speakers sets the latency for the --zylia position survey"
               "refcaps|already trusts every swept speaker's position" "simlatrig|--sim-speaker-latency is a --simulate knob")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 nm)
    list(GET pair 1 why)
    file(READ "${WORK}/${nm}.log" lg)
    string(FIND "${lg}" "${why}" at)
    if(at LESS 0)
      message(FATAL_ERROR "calibrate_cli_test: ${nm}: refused, but not with '${why}' (log: ${WORK}/${nm}.log)")
    endif()
  endforeach()

elseif(MODE STREQUAL "sweep_quality")
  # The interferer is the SIMULATOR's (calib_capture.cpp counts the captures it lands on), so these
  # checks hold the tool against a truth it does not produce. A 250 ms noise burst at the sweep's own
  # level moves a sweep's level by several dB (3.4 dB measured) while its IR SNR stays near 47 dB, so
  # it is the agreement rule that catches it here, not the SNR floor.
  run(clean 0 --layout "${LAYOUT}" --out clean.json ${SIM} --mic ${MIC})
  expect(clean "trims: 52 sweep\\(s\\) for 26 speaker measurement\\(s\\), 0 of them re-swept" "a clean run takes two sweeps a speaker")
  # captures 3, 9 and 30: speaker 1's first sweep, speaker 3's second, and one late in the run
  run(intf 0 --layout "${LAYOUT}" --out intf.json ${SIM} --mic ${MIC} --sim-interferer 3,9,30:0.9:0:noise)
  expect(intf "an interferer \\(a 250 ms noise burst, 0\\.0 dB at the mic" "the interferer is announced")
  string(REGEX MATCHALL "re-swept, sweep [0-9]+: [^\n]*from the last clean sweep" rs "${intf_OUT}")
  list(LENGTH rs nrs)
  if(NOT nrs EQUAL 3)
    message(FATAL_ERROR "calibrate_cli_test: intf: ${nrs} re-sweeps printed, expected one per contaminated capture (3)")
  endif()
  expect(intf "trims: 55 sweep\\(s\\) for 26 speaker measurement\\(s\\), 3 of them re-swept" "exactly the three contaminated captures cost a sweep")
  file(READ "${WORK}/clean.json" jc)
  file(READ "${WORK}/intf.json" ji)
  string(JSON n LENGTH "${jc}" speakers)
  math(EXPR last "${n} - 1")
  foreach(k RANGE ${last})
    foreach(f gain_db delay_ms)
      string(JSON a GET "${jc}" speakers ${k} ${f})
      string(JSON b GET "${ji}" speakers ${k} ${f})
      to_milli(am "${a}")
      to_milli(bm "${b}")
      if(NOT am EQUAL bm)
        message(FATAL_ERROR "calibrate_cli_test: intf: speaker ${k} ${f} ${b} against the clean run's ${a}: a contaminated sweep was used")
      endif()
    endforeach()
  endforeach()
  message(STATUS "sweep_quality: three contaminated captures re-swept, trims equal to the clean run's")

  # a stray sweep louder than the speaker on all five of speaker 1's sweeps (captures 3 to 7): each is
  # flagged outside the window, none is accepted, and the run writes nothing
  run(loud 1 --layout "${LAYOUT}" --out loud.json ${SIM} --mic ${MIC} --sim-interferer 3,4,5,6,7:0.05:6:sweep)
  string(REGEX MATCHALL "speaker  1: sweep [0-9]: the strongest tap \\([0-9.]+ ms\\) is [0-9.]+ dB above the arrival and outside the expected window" ro "${loud_OUT}")
  list(LENGTH ro nro)
  if(NOT nro EQUAL 5)
    message(FATAL_ERROR "calibrate_cli_test: loud: ${nro} sweeps flagged outside the window, expected 5 (log: ${WORK}/loud.log)")
  endif()
  if(EXISTS "${WORK}/loud.json")
    message(FATAL_ERROR "calibrate_cli_test: loud: a run with no clean measurement wrote ${WORK}/loud.json")
  endif()

  # live aiming: reading 3 (capture 4; the --aim-ref sweep is capture 1) carries an 18 dB click that
  # brightens its tilt; the box is held on axis for readings 3 to 5. The peak must be the clean pair's,
  # never the contaminated reading's
  file(READ "${LAYOUT}" js)
  string(JSON js SET "${js}" listening_point_m "[0, 1.448, 0]")
  file(WRITE "${WORK}/live_layout.json" "${js}")
  set(LIVE --layout live_layout.json --live 7 --zylia --simulate --sim-room --aim-ref 16 --sim-aim-steps 25,12,0,0,0,12,25)
  run(live_clean 0 ${LIVE})
  run(live_intf 0 ${LIVE} --sim-interferer 4:0.48:18:click)
  # the clean value is the clean run's reading 4, read off its line, so it does not lean on the peak
  # hold under test
  if(NOT live_clean_OUT MATCHES "\n  #4 [^\n]*\\| tilt ([-+][0-9.]+) dB")
    message(FATAL_ERROR "calibrate_cli_test: live_clean: no tilt on reading 4 (log: ${WORK}/live_clean.log)")
  endif()
  set(pclean "${CMAKE_MATCH_1}")
  if(NOT live_intf_OUT MATCHES "\n  #3 [^\n]*tilt ([-+][0-9.]+) dB[^\n]*NOT HELD: IR peak [0-9.]+ dB over its noise floor")
    message(FATAL_ERROR "calibrate_cli_test: live_intf: the contaminated reading 3 is not marked NOT HELD (log: ${WORK}/live_intf.log)")
  endif()
  set(tbad "${CMAKE_MATCH_1}")
  if(NOT live_intf_OUT MATCHES "live: peak tilt ([-+][0-9.]+) dB at reading 5,")
    message(FATAL_ERROR "calibrate_cli_test: live_intf: the peak is not the clean pair 4-5's (log: ${WORK}/live_intf.log)")
  endif()
  set(pintf "${CMAKE_MATCH_1}")
  string(REGEX REPLACE "^[+]" "" tbad_n "${tbad}")         # to_milli takes no plus sign
  string(REGEX REPLACE "^[+]" "" pclean_n "${pclean}")
  to_milli(mbad "${tbad_n}")
  to_milli(mclean "${pclean_n}")
  math(EXPR lift "${mbad} - ${mclean}")
  message(STATUS "sweep_quality: live: contaminated reading ${tbad} dB, clean peak ${pclean} dB, held peak ${pintf} dB")
  if(lift LESS 200)
    message(FATAL_ERROR "calibrate_cli_test: live_intf: the click lifted the tilt only ${lift} mdB: it would not have set a false peak anyway")
  endif()
  if(NOT pintf STREQUAL pclean)
    message(FATAL_ERROR "calibrate_cli_test: live_intf: held peak ${pintf} dB, the clean run's ${pclean} dB")
  endif()
  expect(live_intf "live: 1 reading\\(s\\) not held by the peak" "the rejected reading is counted")

elseif(MODE STREQUAL "track_settle")
  # no survey and a nonzero --mount-offset: the center is p + R(q) offset, with the simulated stand
  # yawed 30 deg, so a tool that dropped R would land 30 mm off the truth
  track_settled(settle --mount-offset 0.03,-0.12,0.05)
  expect(settle "track: mount offset \\(0\\.0300 -0\\.1200 0\\.0500\\) m in body axes, from --mount-offset" "the offset source is named")
  expect(settle "depends on how Motive orients the rigid body" "a flag offset with no survey says what it depends on")
  # --localize steps through the rows. The scripted stand lingers at the previous row after each
  # retarget (MIC_SIM_LINGER_S, longer than the gate's window plus hold), so a gate on stillness alone
  # opens there and records the previous row again; each row's measured center must sit near ITS row.
  # With that broken gate the run actually dies first, with exit 4: it starts measuring at the old row,
  # the stand then walks to the new one mid-capture, and the bump check stops it. The exit code is
  # checked before the rows, so exit 4 is how this goes red; the row check stands behind it.
  file(WRITE "${WORK}/rows.txt" "1.2 1.2 0.8\n-1.1 1.5 0.9\n0.9 1.8 -1.0\n-1.0 1.0 -0.9\n0.0 2.0 0.0\n")
  run(loc 0 --layout "${LAYOUT}" --out loc.json ${SIM} --localize rows.txt --track-sim --place-timeout 60)
  set(rest "${loc_OUT}")
  foreach(k 1 2 3 4 5)
    string(FIND "${rest}" "placement: localize position ${k}/5: target" at)
    if(at LESS 0)
      message(FATAL_ERROR "calibrate_cli_test: loc: row ${k} was never placed (log: ${WORK}/loc.log)")
    endif()
    string(SUBSTRING "${rest}" ${at} -1 rest)
    set(cut_OUT "${rest}")
    grab(cut "placement: localize position ${k}/5: target \\(${NUM} ${NUM} ${NUM}\\)" row)
    grab(cut "placement: measured center \\(${NUM} ${NUM} ${NUM}\\)" got)
    dist2(d got row)
    # the script settles 5.4 mm off each row: 20 mm allows that and nothing like the next row over
    if(d GREATER 40000)
      message(FATAL_ERROR "calibrate_cli_test: loc: row ${k} ${row_s} was recorded at ${got_s} (log: ${WORK}/loc.log)")
    endif()
    string(SUBSTRING "${rest}" 1 -1 rest)
  endforeach()

elseif(MODE STREQUAL "track_ring")
  # --mount-offset ring: the simulated MODELDEF carries 4 markers at 0/90/180/225 deg on a 60 mm ring,
  # pivot at their centroid (11.5 mm off the ring's axis); the offset must be the ring's center
  track_settled(ring --mount-offset ring)
  expect(ring "the center of the 4-marker ring: 11\\.5 mm from\ntrack: the marker centroid, radius 60\\.0 mm"
         "the ring fit is reported with its centroid distance and radius")
  expect(ring "sits at the array center's height" "the ring's height assumption is stated")

elseif(MODE STREQUAL "track_bump")
  # knocked 15 mm after the fifth capture: the run stops, exit 4, nothing written
  run(bump 4 --layout "${LAYOUT}" --out bump.json ${SIM} --zylia --trims --track-sim --track-sim-bump 5)
  if(NOT "${bump_OUT}" MATCHES "placement: measured center")
    message(FATAL_ERROR "calibrate_cli_test: bump: the run never placed the mic (log: ${WORK}/bump.log)")
  endif()
  file(READ "${WORK}/bump.log" blog)
  if(NOT blog MATCHES "calibrate: BUMP: the ZM-1 moved 1[4-6]\\.[0-9] mm during the trim run, after speaker 4 \\(limit 5\\.0 mm")
    message(FATAL_ERROR "calibrate_cli_test: bump: no bump message naming the move and the speaker (log: ${WORK}/bump.log)")
  endif()
  if(EXISTS "${WORK}/bump.json")
    message(FATAL_ERROR "calibrate_cli_test: bump: trims were written across a moved mic")
  endif()
  # the same knock on --verify
  run(vbump 4 --layout "${LAYOUT}" ${SIM} --zylia --verify --track-sim --track-sim-bump 3)

elseif(MODE STREQUAL "track_refuse")
  # the --zylia position survey turns DOAs into room directions: tracked, it needs a BODY-FRAME survey
  run(nosurvey 2 --layout "${LAYOUT}" --out ns.json ${SIM} --zylia --track-sim --mic 0 1.5 0)
  file(READ "${WORK}/nosurvey.log" nlog)
  if(NOT nlog MATCHES "needs --survey <body-frame survey>" OR NOT nlog MATCHES "a BODY-FRAME survey")
    message(FATAL_ERROR "calibrate_cli_test: nosurvey: the refusal does not say why (log: ${WORK}/nosurvey.log)")
  endif()
  # ...a room-axes survey is refused for it too (the fixture below with its frame field dropped)
  file(READ "${FIXDIR}/calib_cli_track_survey_fixture.json" fx)
  string(JSON fx REMOVE "${fx}" frame)
  string(JSON fx REMOVE "${fx}" mount_offset_m)
  file(WRITE "${WORK}/room_axes.json" "${fx}")
  run(roomaxes 2 --layout "${LAYOUT}" --out ra.json ${SIM} --zylia --track-sim --mic 0 1.5 0 --survey room_axes.json)
  file(READ "${WORK}/roomaxes.log" rlog)
  if(NOT rlog MATCHES "is in ROOM axes, not the mount's body frame")
    message(FATAL_ERROR "calibrate_cli_test: roomaxes: a room-axes survey was not refused for the tracked position survey")
  endif()
  # ...but the trims only need the center: the same room-axes survey is accepted there
  run(roomtrims 0 --layout "${LAYOUT}" --out rt.json ${SIM} --zylia --trims --track-sim --survey room_axes.json)
  expect(roomtrims "track: room-axes survey: installed for its channel order and geometry, NOT re-aimed" "a room-axes survey rides along")
  # live aiming with no body-frame survey: the tilt meter runs, the position readout is refused
  file(READ "${LAYOUT}" js)
  string(JSON js SET "${js}" listening_point_m "[0, 1.448, 0]")
  file(WRITE "${WORK}/live_layout.json" "${js}")
  run(live 0 --layout live_layout.json --live 7 --zylia --simulate --track-sim --sweeps 2)
  expect(live "the POSITION readout is off" "the live mode says why there is no position")
  expect(live "\n  #1 +position: n/a \\(no body-frame survey\\) \\| tilt [-+][0-9.]+ dB" "reading 1 has a tilt and no position")
  # flags that make no sense
  run(nosim 2 --layout "${LAYOUT}" --track-sim --zylia --trims)
  run(check 2 --layout "${LAYOUT}" ${SIM} --track 3 --check)
  run(tolnotrack 2 --layout "${LAYOUT}" ${SIM} --place-tol-mm 5)
  run(gridnomic 2 --layout "${LAYOUT}" ${SIM} --track-sim --room-eq-grid)

elseif(MODE STREQUAL "track_survey")
  # A BODY-FRAME survey carries the offset (here 0.02 -0.11 0.04 in body axes) and a table the tool
  # re-aims for the yawed stand; the tracked position survey then writes every speaker where the
  # layout has it. (In simulate the captures come from the re-aimed table the solve reads, so this
  # checks the center and the survey path, not the re-aim itself: see validate_track.)
  file(READ "${FIXDIR}/calib_cli_track_survey_fixture.json" fx)
  file(WRITE "${WORK}/body.json" "${fx}")
  run(survey 0 --layout "${LAYOUT}" --out surveyed.json ${SIM} --zylia --track-sim --mic 0 1.5 0 --survey body.json)
  expect(survey "track: mount offset \\(0\\.0200 -0\\.1100 0\\.0400\\) m in body axes, from the survey" "the survey's offset is used")
  expect(survey "track: body-frame survey: the capsule table follows the stand" "the table is re-aimed")
  expect(survey "capsule table re-aimed" "the placement re-aimed it")
  grab(survey "placement: measured center \\(${NUM} ${NUM} ${NUM}\\)" meas)
  grab(survey "track-sim: true center \\(${NUM} ${NUM} ${NUM}\\)" truth)
  dist2(d meas truth)
  if(d GREATER 100)
    message(FATAL_ERROR "calibrate_cli_test: survey: measured ${meas_s} is not the true center ${truth_s}")
  endif()
  file(READ "${LAYOUT}" jl)
  file(READ "${WORK}/surveyed.json" js)
  string(JSON n LENGTH "${jl}" speakers)
  math(EXPR last "${n} - 1")
  set(worst 0)
  foreach(k RANGE ${last})
    foreach(a 0 1 2)
      string(JSON pa GET "${jl}" speakers ${k} position ${a})
      string(JSON pb GET "${js}" speakers ${k} position ${a})
      to_milli(ma "${pa}")
      to_milli(mb "${pb}")
      math(EXPR dd "${ma} - ${mb}")
      if(dd LESS 0)
        math(EXPR dd "-${dd}")
      endif()
      if(dd GREATER worst)
        set(worst ${dd})
      endif()
    endforeach()
  endforeach()
  message(STATUS "tracked position survey: worst speaker coordinate off the layout ${worst} mm")
  if(worst GREATER 3)
    message(FATAL_ERROR "calibrate_cli_test: survey: a speaker landed ${worst} mm off the layout")
  endif()

elseif(MODE STREQUAL "track_twist")
  # The scripted stand turns MIC_SIM_TWIST_DEG (2 deg) about room vertical through the array center after
  # the Nth capture. The center does not move, so a center-only check cannot see it; only the modes that
  # read room DIRECTIONS may stop on it. Each run below must have PLACED the mic first, so an exit code
  # that comes from a gate that never opened (exit 1) cannot pass for the verdict under test.
  file(READ "${FIXDIR}/calib_cli_track_survey_fixture.json" fx)
  file(WRITE "${WORK}/body.json" "${fx}")

  # twisted_stop(<name> <what> <unit> <idx>): the run placed, then stopped on a TURN of about 2 deg with
  # its center in place, at the capture the script twisted after
  function(twisted_stop name what unit idx)
    if(NOT "${${name}_OUT}" MATCHES "placement: measured center")
      message(FATAL_ERROR "calibrate_cli_test: ${name}: the run never placed the mic (log: ${WORK}/${name}.log)")
    endif()
    if(NOT "${${name}_OUT}" MATCHES "and turning under 0\\.30 deg")
      message(FATAL_ERROR "calibrate_cli_test: ${name}: the gate has no orientation term in a direction mode (log: ${WORK}/${name}.log)")
    endif()
    file(READ "${WORK}/${name}.log" lg)
    if(NOT lg MATCHES "calibrate: BUMP: the ZM-1 turned ([0-9]+\\.[0-9]+) deg during the ${what}, after ${unit} ${idx} \\(limit 0\\.30 deg\\), its\n +center ([0-9]+\\.[0-9]) mm from where it was taken")
      message(FATAL_ERROR "calibrate_cli_test: ${name}: no turn message naming the turn, the ${unit} and the limit (log: ${WORK}/${name}.log)")
    endif()
    set(tdeg "${CMAKE_MATCH_1}")
    set(cmm "${CMAKE_MATCH_2}")
    to_milli(mt "${tdeg}")
    to_milli(mc "${cmm}")
    message(STATUS "${name}: stopped on a turn of ${tdeg} deg (the script's 2.0) with the center ${cmm} mm from the take")
    if(mt LESS 1900 OR mt GREATER 2100)
      message(FATAL_ERROR "calibrate_cli_test: ${name}: the turn read ${tdeg} deg, not the script's 2 deg")
    endif()
    if(mc GREATER 1000)
      message(FATAL_ERROR "calibrate_cli_test: ${name}: the center moved ${cmm} mm: the twist was not about the center")
    endif()
  endfunction()

  # 1. the --zylia position survey: a direction mode. Twisted after the 5th capture: stops after speaker 4.
  run(survey 4 --layout "${LAYOUT}" --out surveyed.json ${SIM} --zylia --track-sim --mic 0 1.5 0 --survey body.json
      --track-sim-twist 5)
  twisted_stop(survey "position survey" speaker 4)
  if(EXISTS "${WORK}/surveyed.json")
    message(FATAL_ERROR "calibrate_cli_test: survey: positions were written across a turned array")
  endif()

  # 2. --zylia --trims with the SAME body-frame survey and the same twist: the trims read only the center,
  #    so the run must finish. The survey is deliberately the same: what selects the check is the MODE, not
  #    whether an orientation is available.
  run(trims 0 --layout "${LAYOUT}" --out trims.json ${SIM} --zylia --trims --track-sim --survey body.json
      --track-sim-twist 5)
  expect(trims "placement: measured center" "the trim run placed the mic")
  expect(trims "placement: trims: target \\([^)]*\\), tolerance 10\\.0 mm, still within 2\\.0 mm for"
         "the trim run's gate has no orientation term")
  expect(trims "track-sim: the stand was twisted 2\\.0 deg about the array center \\(--track-sim-twist\\); this mode reads only the center"
         "the script's own record says the stand WAS twisted during the trim run")
  expect(trims "\n +26 bump check\\(s\\), the largest move 0\\.[0-9] mm \\(limit 5\\.0 mm\\), the largest turn (1\\.9[0-9]|2\\.[01][0-9]) deg \\(unchecked"
         "every capture was bump-checked, the center never moved, and the tool SAW the 2 deg turn and let it pass")
  if(NOT EXISTS "${WORK}/trims.json")
    message(FATAL_ERROR "calibrate_cli_test: trims: no trims were written: the twist stopped a center-only run")
  endif()

  # 3. live aiming with its position readout on (a body-frame survey): a direction mode. Twisted after the
  #    3rd reading: stops after reading 3, and each reading line shows the turn so far.
  file(READ "${LAYOUT}" js)
  string(JSON js SET "${js}" listening_point_m "[0, 1.448, 0]")
  file(WRITE "${WORK}/live_layout.json" "${js}")
  run(live 4 --layout live_layout.json --live 7 --zylia --simulate --track-sim --survey body.json --sweeps 6
      --track-sim-twist 3)
  twisted_stop(live "live aiming" reading 3)
  expect(live "\n  #2 +pos [^\n]*\\| turned 0\\.0[0-9] deg\n" "a reading line shows the turn since the take")

  # 4. ...and with no body-frame survey the live mode is the tilt meter alone, which reads the center:
  #    the same twist runs every sweep
  run(tilt 0 --layout live_layout.json --live 7 --zylia --simulate --track-sim --sweeps 6 --track-sim-twist 3)
  expect(tilt "\n  #6 +position: n/a" "the tilt meter ran all six sweeps")
  expect(tilt "track-sim: the stand was twisted 2\\.0 deg about the array center" "the stand WAS twisted")
  expect(tilt "the largest turn (1\\.9[0-9]|2\\.[01][0-9]) deg \\(unchecked" "the tool saw the turn and let it pass")

  # the knob acts on the simulated stand only
  run(twistnosim 2 --layout "${LAYOUT}" ${SIM} --zylia --trims --track-sim-twist 3)

elseif(MODE STREQUAL "grid_rows")
  # --room-eq-grid rows.txt: one placement per row, tracked, ONE grid written. The scripted stand settles
  # 5.4 mm off every row, so a key equal to the row is caught as surely as a key from the wrong row.
  file(WRITE "${WORK}/rows.txt" "0.5 1.4 0.5\n-0.5 1.4 0.5\n0.0 1.4 -0.6\n")
  set(want_rows "(0.500 1.400 0.500)" "(-0.500 1.400 0.500)" "(0.000 1.400 -0.600)")   # the file above, typed again
  run(grid 0 --layout "${LAYOUT}" --out grid.json ${SIM} --room-eq-grid rows.txt --track-sim --place-timeout 60)
  if(NOT EXISTS "${WORK}/grid.json")
    message(FATAL_ERROR "calibrate_cli_test: grid: no layout was written")
  endif()
  file(READ "${WORK}/grid.json" gj)
  string(JSON np LENGTH "${gj}" room_eq_grid)
  if(NOT np EQUAL 3)
    message(FATAL_ERROR "calibrate_cli_test: grid: ${np} grid positions written, expected 3 (log: ${WORK}/grid.log)")
  endif()
  set(rest "${grid_OUT}")
  foreach(k 1 2 3)
    string(FIND "${rest}" "placement: room-eq-grid row ${k}/3: target" at)
    if(at LESS 0)
      message(FATAL_ERROR "calibrate_cli_test: grid: row ${k} was never placed (log: ${WORK}/grid.log)")
    endif()
    string(SUBSTRING "${rest}" ${at} -1 rest)
    set(cut_OUT "${rest}")
    grab(cut "placement: room-eq-grid row ${k}/3: target \\(${NUM} ${NUM} ${NUM}\\)" row)
    math(EXPR i "${k} - 1")
    list(GET want_rows ${i} want)
    if(NOT row_s STREQUAL want)
      message(FATAL_ERROR "calibrate_cli_test: grid: row ${k} was placed at ${row_s}, but the file's row ${k} is ${want}")
    endif()
    grab(cut "placement: measured center \\(${NUM} ${NUM} ${NUM}\\)" meas)
    grab(cut "track-sim: true center \\(${NUM} ${NUM} ${NUM}\\)" truth)
    # the key the FILE carries for this row (the writer rounds to 1 mm)
    math(EXPR i "${k} - 1")
    foreach(a 0 1 2)
      string(JSON v GET "${gj}" room_eq_grid ${i} position ${a})
      if(NOT v MATCHES "\\.")
        set(v "${v}.0")
      endif()
      list(APPEND kv "${v}")
    endforeach()
    list(GET kv 0 k0)
    list(GET kv 1 k1)
    list(GET kv 2 k2)
    set(kv "")
    set(key_OUT "(${k0} ${k1} ${k2})")
    grab(key "\\(${NUM} ${NUM} ${NUM}\\)" key)
    dist2(d_key_meas key meas)
    dist2(d_key_row key row)
    dist2(d_meas_truth meas truth)
    message(STATUS "grid row ${k}: planned ${row_s}, measured ${meas_s}, true ${truth_s}, key ${key_s}")
    # within the writer's 1 mm rounding of the measured center (0.5 mm per axis)
    if(d_key_meas GREATER 100)
      message(FATAL_ERROR "calibrate_cli_test: grid: row ${k}'s key ${key_s} is not its measured center ${meas_s}")
    endif()
    # the stand settles 5.4 mm off the row: a key within 3 mm of it is the row, not a measurement
    if(d_key_row LESS 900)
      message(FATAL_ERROR "calibrate_cli_test: grid: row ${k}'s key ${key_s} is the planned row ${row_s}, not where the mic stood")
    endif()
    if(d_meas_truth GREATER 100)
      message(FATAL_ERROR "calibrate_cli_test: grid: row ${k} measured ${meas_s}, not the stand's true center ${truth_s}")
    endif()
    string(SUBSTRING "${rest}" 1 -1 rest)
  endforeach()
  expect(grid "calibrate: merged 3 position\\(s\\) into room_eq_grid" "one write, after the last row")
  # the rows mode writes the grid and nothing else: the trims stay what the layout had
  file(READ "${LAYOUT}" jl)
  foreach(f gain_db delay_ms)
    string(JSON a GET "${jl}" speakers 5 ${f})
    string(JSON b GET "${gj}" speakers 5 ${f})
    if(NOT a STREQUAL b)
      message(FATAL_ERROR "calibrate_cli_test: grid: speaker 5's ${f} changed (${a} -> ${b}): the rows mode must not write trims")
    endif()
  endforeach()

  # capacity: 17 rows are refused before any sweep (BWA_RQ_GRID_MAX is 16)
  set(r17 "")
  foreach(x -0.9 -0.6 -0.3 0.0 0.3 0.6 0.9)
    foreach(z -0.3 0.3)
      string(APPEND r17 "${x} 1.0 ${z}\n")
    endforeach()
  endforeach()
  set(r14 "${r17}")
  string(APPEND r17 "-0.6 1.0 0.9\n0.0 1.0 0.9\n0.6 1.0 0.9\n")
  file(WRITE "${WORK}/rows17.txt" "${r17}")
  file(WRITE "${WORK}/rows14.txt" "${r14}")
  run(full 2 --layout "${LAYOUT}" --out full.json ${SIM} --room-eq-grid rows17.txt)
  file(READ "${WORK}/full.log" flog)
  if(NOT flog MATCHES "rows17\\.txt: 17 rows, and room_eq_grid holds 16 positions")
    message(FATAL_ERROR "calibrate_cli_test: full: 17 rows were not refused by name (log: ${WORK}/full.log)")
  endif()
  # ...and the grid already in the file counts: 3 there, 14 new rows away from them = 17
  run(fuller 2 --layout grid.json --out fuller.json ${SIM} --room-eq-grid rows14.txt)
  file(READ "${WORK}/fuller.log" flog)
  if(NOT flog MATCHES "already holds 3 position\\(s\\), 3 of them away from\n +every row, and 14 rows would make 17")
    message(FATAL_ERROR "calibrate_cli_test: fuller: the existing grid was not counted against the cap (log: ${WORK}/fuller.log)")
  endif()
  if(EXISTS "${WORK}/full.json" OR EXISTS "${WORK}/fuller.json")
    message(FATAL_ERROR "calibrate_cli_test: a refused grid plan wrote a layout")
  endif()
  # rows a gate could confuse are refused: tracked, each key may land a whole 100 mm tolerance off its row
  file(WRITE "${WORK}/close.txt" "0.0 1.4 0.0\n0.2 1.4 0.0\n")
  run(close 2 --layout "${LAYOUT}" --out close.json ${SIM} --room-eq-grid close.txt --track-sim)
  file(READ "${WORK}/close.log" clog)
  if(NOT clog MATCHES "rows 1 and 2 are 200 mm apart; keep rows at least 250 mm")
    message(FATAL_ERROR "calibrate_cli_test: close: rows 200 mm apart were not refused (log: ${WORK}/close.log)")
  endif()
  # a bump in the second row stops the run and writes nothing, the first row's measurement included
  run(gbump 4 --layout "${LAYOUT}" --out gbump.json ${SIM} --room-eq-grid rows.txt --track-sim --track-sim-bump 30)
  file(READ "${WORK}/gbump.log" blog)
  if(NOT blog MATCHES "BUMP: the ZM-1 moved [0-9.]+ mm during the room-eq-grid row 2/3, after speaker 3")
    message(FATAL_ERROR "calibrate_cli_test: gbump: no bump named in row 2 (log: ${WORK}/gbump.log)")
  endif()
  if(EXISTS "${WORK}/gbump.json")
    message(FATAL_ERROR "calibrate_cli_test: gbump: a grid was written across a moved mic")
  endif()
  # the rows and --mic say the same thing twice
  run(rowsmic 2 --layout "${LAYOUT}" ${SIM} --room-eq-grid rows.txt --mic 0 1.4 0)
  # with the ZM-1 (the rig's only mic) the rows are their own mode: bare --zylia must not read them as
  # the position survey, and no --trims is needed. One untracked row: the row is the key.
  file(WRITE "${WORK}/row1.txt" "0.2 1.4 -0.2\n")
  run(zrow 0 --layout "${LAYOUT}" --out zrow.json ${SIM} --zylia --room-eq-grid row1.txt)
  expect(zrow "zylia: the ZM-1 is the grid mic" "the ZM-1 reads the grid")
  file(READ "${WORK}/zrow.json" zj)
  string(JSON zn LENGTH "${zj}" room_eq_grid)
  string(JSON zx GET "${zj}" room_eq_grid 0 position 0)
  string(JSON zy GET "${zj}" room_eq_grid 0 position 1)
  string(JSON zz GET "${zj}" room_eq_grid 0 position 2)
  set(key_OUT "(${zx} ${zy} ${zz})")
  grab(key "\\(${NUM} ${NUM} ${NUM}\\)" zk)
  set(row_OUT "(0.2000 1.4000 -0.2000)")
  grab(row "\\(${NUM} ${NUM} ${NUM}\\)" zr)
  dist2(dz zk zr)
  if(NOT zn EQUAL 1 OR dz GREATER 4)
    message(FATAL_ERROR "calibrate_cli_test: zrow: ${zn} grid position(s), key ${zk_s}: expected one, at the row (0.2 1.4 -0.2)")
  endif()
  # the bare form is unchanged: one --mic, one entry
  run(bare 0 --layout "${LAYOUT}" --out bare.json ${SIM} --room-eq-grid --mic 0.3 1.4 0.2)
  file(READ "${WORK}/bare.json" bj)
  string(JSON nb LENGTH "${bj}" room_eq_grid)
  if(NOT nb EQUAL 1)
    message(FATAL_ERROR "calibrate_cli_test: bare: the --mic form wrote ${nb} grid positions, expected 1")
  endif()

elseif(MODE STREQUAL "zylia_localize")
  # --localize rows.txt --zylia on the scripted stand (yawed 30 deg, 1.5 deg off level). Each range is the
  # ARRAY CENTER's arrival from the 19 capsules, which does not turn with the array, so no survey is given.
  # The captures come from the simulated PHYSICAL ZM-1 (calib_sim_zm1_room: the built-in table scaled and
  # turned 40 deg inside its mount, then by the stand), never from the built-in table the run reads, so a
  # range that depended on the orientation would land off the truth. Anechoic: 650 captures of 19 capsules
  # in a room take three times as long, and the room is not what this checks.
  file(WRITE "${WORK}/rows.txt" "1.2 1.2 0.8\n-1.1 1.5 0.9\n0.9 1.8 -1.0\n-1.0 1.0 -0.9\n0.0 2.0 0.0\n")
  run(loc 0 --layout "${LAYOUT}" --out loc.json --simulate --zylia --localize rows.txt --track-sim --place-timeout 60)
  expect(loc "localize: the ZM-1: each range is the arrival at the array center" "the run says which arrival it ranges with")
  expect(loc "the captures come from the simulated physical ZM-1" "the simulator is the physical array, not the table")
  expect(loc "mount yaw -30\\.0 deg, tilt 1\\.5 deg" "the stand is turned, so an orientation-dependent range would show")
  expect(loc "localize: wrote 26 positions to loc\\.json" "the positions are written")
  # Every speaker against the layout it was simulated from. Five rows give each speaker's 4-unknown solve
  # one spare equation, so the geometry amplifies what is left: the tracker's 0.15 mm jitter, and the center
  # arrival's plane-wave model, which reads late by about R^2 / (3 d c) (0.4 mm at 2 m, 1 mm at 0.8 m). That
  # lands the worst speaker (2, the corner the rows see from one side) about 7 mm off and the rest within
  # 2 mm. One capsule's arrival instead of the center's is off by up to R/c (49 mm) with the direction, and
  # puts speakers several centimeters off. The bounds: worst 12 mm, RMS 3 mm.
  file(READ "${LAYOUT}" jl)
  file(READ "${WORK}/loc.json" jo)
  string(JSON n LENGTH "${jl}" speakers)
  math(EXPR last "${n} - 1")
  set(worst 0)
  set(wk 0)
  set(sum 0)
  foreach(k RANGE ${last})
    set(d2 0)
    foreach(a 0 1 2)
      string(JSON pa GET "${jl}" speakers ${k} position ${a})
      string(JSON pb GET "${jo}" speakers ${k} position ${a})
      to_milli(ma "${pa}")
      to_milli(mb "${pb}")
      math(EXPR d2 "${d2} + (${ma} - ${mb}) * (${ma} - ${mb})")
    endforeach()
    math(EXPR sum "${sum} + ${d2}")
    if(d2 GREATER worst)
      set(worst ${d2})
      set(wk ${k})
    endif()
  endforeach()
  math(EXPR ms "${sum} / ${n}")
  message(STATUS "zylia localize: worst speaker ${wk} at ${worst} mm^2 off the layout, mean square ${ms} mm^2 over ${n}")
  if(worst GREATER 144 OR ms GREATER 9)
    message(FATAL_ERROR "calibrate_cli_test: loc: the positions are off the layout: worst speaker ${wk} by ${worst} mm^2 (limit 144), mean square ${ms} mm^2 (limit 9)")
  endif()
  # the ZM-1 localize takes the plan the way the omni one does
  expect(loc "localize: recorded the plan for 26 of 26 speaker" "the first survey keeps the plan")
  # what --zylia still refuses beside --localize
  run(zcheck 2 --layout "${LAYOUT}" --simulate --zylia --check)
  run(ztrims 2 --layout "${LAYOUT}" --simulate --zylia --trims --localize rows.txt)

elseif(MODE STREQUAL "capsule_survey")
  # --capsule-survey: the capsule table from one placement, the speakers as the known sources. The truth is
  # the simulated PHYSICAL ZM-1 (calib_sim_zm1_*: 49.5 mm, turned 40 deg and tilted 3 deg inside its mount,
  # then by the stand), printed by the simulator's own quaternion code; the run reads it nowhere, and it is
  # not the built-in table, so a run that saved the table it started from fails on every capsule.

  # 1. Tracked, in the simulated room, told the WRONG mount offset: the stand's true offset is 17.7 mm away
  #    (--track-sim-offset). Every center the tracker gives is off by that, so the gate is widened to 40 mm
  #    and the checks below decide. The acoustic offset must find the true one.
  set(TOLD 0.02,-0.11,0.04)
  set(TRUE_OFF "0.031 -0.118 0.052")
  run(tracked 0 --layout "${LAYOUT}" ${SIM} --zylia --capsule-survey body.json --mic 0.1 1.45 0.05 --track-sim
      --mount-offset ${TOLD} --track-sim-offset 0.031,-0.118,0.052 --place-tol-mm 40 --place-timeout 60)
  expect(tracked "track-sim: true orientation at the take, q = " "the simulator prints the stand's true orientation")
  expect(tracked "capsule survey: residual 0\\.[0-9][0-9] us, radius 49\\.[45][0-9] mm, spread [0-9.]+"
         "a sub-microsecond residual and the simulated array's 49.5 mm radius, measured")
  expect(tracked "capsule survey: wrote a BODY-FRAME survey to body\\.json \\(the acoustic mount offset\\)" "a body-frame file")
  # the table: the saved file, reloaded through zylia_survey_load and re-aimed at the take's orientation
  # (what every direction mode does with it), against the simulator's room-axes truth
  grab_caps(tracked "sim-truth: cap" truth)
  grab_caps(tracked "reload: cap" got)
  caps_worst(w truth got)
  message(STATUS "capsule survey, tracked: worst capsule ${w_i} off the truth by ${w} um^2")
  # 0.1 mm: the simulation lands within 0.03 mm (the stand's 0.03 deg wobble between the take's mean
  # orientation and the truth at one instant); a rotation transposed into the body frame is 60 deg of
  # error on a 49.5 mm sphere, 50 mm per capsule
  if(w GREATER 10000)
    message(FATAL_ERROR "calibrate_cli_test: tracked: the reloaded, re-aimed table is ${w} um^2 off the truth at capsule ${w_i} (limit 10000)")
  endif()
  # the acoustic offset against the stand's TRUE offset, which the run was not given
  grab(tracked "acoustic \\(${NUM} ${NUM} ${NUM}\\) m:" ac)
  set(want_OUT "(${TRUE_OFF})")
  grab(want "\\(${NUM} ${NUM} ${NUM}\\)" tru)
  dist2(d_off ac tru)
  message(STATUS "capsule survey, tracked: acoustic offset ${ac_s}, true ${tru_s}, told (${TOLD}): ${d_off} (0.1 mm)^2 off")
  # 1 mm (100 in tenths squared): the acoustic center lands within 0.1 mm in simulation; the told offset
  # is 17.7 mm away and a sign error doubles that
  if(d_off GREATER 100)
    message(FATAL_ERROR "calibrate_cli_test: tracked: the acoustic offset ${ac_s} is not the stand's true offset ${tru_s}")
  endif()
  expect(tracked "WARNING the acoustic center and the tracked one disagree by 1[78]\\.[0-9] mm" "the disagreement is called out")
  # the file carries that offset, and with it the center the next placement computes is the TRUE one
  file(READ "${WORK}/body.json" bj)
  string(JSON fr GET "${bj}" frame)
  if(NOT fr STREQUAL "body")
    message(FATAL_ERROR "calibrate_cli_test: tracked: the survey's frame is '${fr}', not body")
  endif()
  foreach(a 0 1 2)
    string(JSON v GET "${bj}" mount_offset_m ${a})
    list(APPEND fo "${v}")
  endforeach()
  list(GET fo 0 f0)
  list(GET fo 1 f1)
  list(GET fo 2 f2)
  if(f0 MATCHES "[eE]" OR f1 MATCHES "[eE]" OR f2 MATCHES "[eE]")
    message(FATAL_ERROR "calibrate_cli_test: tracked: the file's offset (${f0} ${f1} ${f2}) is not plain decimals")
  endif()
  set(file_OUT "(${f0} ${f1} ${f2})")
  grab(file "\\(${NUM} ${NUM} ${NUM}\\)" fo)
  dist2(d_file fo tru)
  if(d_file GREATER 100)
    message(FATAL_ERROR "calibrate_cli_test: tracked: the file's mount offset ${fo_s} is not the true one ${tru_s}")
  endif()
  grab(tracked "reload: center from the saved offset at the take: \\(${NUM} ${NUM} ${NUM}\\)" rc)
  grab(tracked "track-sim: true center \\(${NUM} ${NUM} ${NUM}\\)" tc)
  dist2(d_c rc tc)
  if(d_c GREATER 100)
    message(FATAL_ERROR "calibrate_cli_test: tracked: the saved offset puts the center at ${rc_s}, not the true ${tc_s}")
  endif()

  # 2. Tracked, anechoic, told the RIGHT offset: the two centers agree and nothing is called out
  run(agree 0 --layout "${LAYOUT}" --simulate --zylia --capsule-survey agree.json --mic 0.1 1.45 0.05 --track-sim
      --mount-offset ${TOLD} --place-timeout 60)
  if(NOT agree_OUT MATCHES "acoustic \\([^)]*\\) m:\n +([0-9]+)\\.([0-9]) mm apart")
    message(FATAL_ERROR "calibrate_cli_test: agree: no offset comparison (log: ${WORK}/agree.log)")
  endif()
  if(CMAKE_MATCH_1 GREATER 0)
    message(FATAL_ERROR "calibrate_cli_test: agree: the right offset reads ${CMAKE_MATCH_1}.${CMAKE_MATCH_2} mm off the acoustic one")
  endif()
  if(agree_OUT MATCHES "WARNING the acoustic center")
    message(FATAL_ERROR "calibrate_cli_test: agree: a right offset was called out")
  endif()

  # 3. Untracked, anechoic, the taped --mic 19.7 mm off where the array really is (--sim-center): a
  #    ROOM-AXES file, the table against the truth, and the acoustic center on the real one
  run(untracked 0 --layout "${LAYOUT}" --simulate --zylia --capsule-survey room.json --mic 0.1 1.45 0.05
      --sim-center 0.115,1.44,0.058)
  expect(untracked "wrote a ROOM-AXES survey to room\\.json" "a room-axes file")
  file(READ "${WORK}/room.json" rj)
  string(JSON fr ERROR_VARIABLE ferr GET "${rj}" frame)
  if(NOT ferr)
    message(FATAL_ERROR "calibrate_cli_test: untracked: the room-axes survey carries a frame ('${fr}')")
  endif()
  grab_caps(untracked "sim-truth: cap" ut)
  grab_caps(untracked "reload: cap" ug)
  caps_worst(uw ut ug)
  message(STATUS "capsule survey, untracked: worst capsule ${uw_i} off the truth by ${uw} um^2")
  if(uw GREATER 10000)
    message(FATAL_ERROR "calibrate_cli_test: untracked: the saved table is ${uw} um^2 off the truth at capsule ${uw_i}")
  endif()
  grab(untracked "capsule survey: acoustic center \\(${NUM} ${NUM} ${NUM}\\)" uc)
  set(sc_OUT "(0.115 1.44 0.058)")
  grab(sc "\\(${NUM} ${NUM} ${NUM}\\)" sc)
  dist2(d_uc uc sc)
  if(d_uc GREATER 100)
    message(FATAL_ERROR "calibrate_cli_test: untracked: the acoustic center ${uc_s} is not where the array is, ${sc_s}")
  endif()

  # Leave-one-out on every clean run above: every speaker checked, none flagged, and the file written
  foreach(nm tracked agree untracked)
    expect(${nm} "capsule survey: leave-one-out, each speaker against the table the other 25 solve to"
           "leave-one-out ran over the 26 speakers")
    string(REGEX MATCHALL "\n  spk [ 0-9]+: held out " held "${${nm}_OUT}")
    list(LENGTH held nheld)
    if(NOT nheld EQUAL 26)
      message(FATAL_ERROR "calibrate_cli_test: ${nm}: leave-one-out scored ${nheld} speakers, not 26 (log: ${WORK}/${nm}.log)")
    endif()
    if("${${nm}_OUT}" MATCHES "FLAGGED|flagged speaker|\n  spk [ 0-9]+: unchecked")
      message(FATAL_ERROR "calibrate_cli_test: ${nm}: a clean survey flagged or skipped a speaker (log: ${WORK}/${nm}.log)")
    endif()
  endforeach()

  # 5. The range check. Speaker 15's layout position 40 mm off its truth RADIALLY, straight out along the
  #    line from the array's true center (0.115, 1.44, 0.058) through (1.5, 1.5, 0): every capsule's
  #    direction to it is unchanged, so leave-one-out cannot see it (held out about 0.02 us, against its 3 us
  #    floor), but its range is 40 mm long, which bends the center's trilateration (4.9 mm unrefused). The
  #    range check refuses it by name, exit 6, nothing written; nobody else is flagged.
  file(READ "${LAYOUT}" rj0)
  string(JSON rj0 SET "${rj0}" speakers 15 position "[1.5399, 1.5017, -0.0017]")
  file(WRITE "${WORK}/radial.json" "${rj0}")
  run(radial 6 --layout radial.json --sim-truth "${LAYOUT}" --simulate --zylia --mic 0.1 1.45 0.05
      --sim-center 0.115,1.44,0.058 --capsule-survey radial_out.json)
  if(NOT radial_OUT MATCHES "\n  spk 15: held out +([0-9]+)\\.([0-9]+) us, the rest [0-9.]+ us\n")
    message(FATAL_ERROR "calibrate_cli_test: radial: no unflagged leave-one-out line for speaker 15 (log: ${WORK}/radial.log)")
  endif()
  set(loo15 "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
  if(CMAKE_MATCH_1 GREATER_EQUAL 3)
    message(FATAL_ERROR "calibrate_cli_test: radial: speaker 15 held out at ${loo15} us, not under the 3 us floor: the radial move is not one leave-one-out misses")
  endif()
  if(radial_OUT MATCHES "FLAGGED: it does not fit the rest|leave-one-out flagged")
    message(FATAL_ERROR "calibrate_cli_test: radial: leave-one-out flagged something (log: ${WORK}/radial.log)")
  endif()
  if(NOT radial_OUT MATCHES "\n  spk 15: [0-9.]+ m, \\+[0-9.]+ mm, \\+(3[0-9]\\.[0-9]+) from the median   RANGE FLAGGED")
    message(FATAL_ERROR "calibrate_cli_test: radial: the range check did not flag speaker 15 about 35 mm long (log: ${WORK}/radial.log)")
  endif()
  set(rng15 "${CMAKE_MATCH_1}")
  string(REGEX MATCHALL "RANGE FLAGGED" rfl "${radial_OUT}")
  list(LENGTH rfl nrfl)
  if(NOT nrfl EQUAL 1)
    message(FATAL_ERROR "calibrate_cli_test: radial: the range check flagged ${nrfl} speakers, not speaker 15 alone (log: ${WORK}/radial.log)")
  endif()
  file(READ "${WORK}/radial.log" lg)
  if(NOT lg MATCHES "1 speaker\\(s\\) flagged, 15 \\(range\\); the worst speaker 15" OR NOT lg MATCHES "--speakers 0-14,16-25")
    message(FATAL_ERROR "calibrate_cli_test: radial: the refusal does not name speaker 15 by its range and the way out (log: ${WORK}/radial.log)")
  endif()
  if(EXISTS "${WORK}/radial_out.json")
    message(FATAL_ERROR "calibrate_cli_test: radial: the refused survey was written")
  endif()
  message(STATUS "capsule survey, speaker 15 40 mm out radially: leave-one-out ${loo15} us (floor 3), range +${rng15} mm off the median (flag 10)")

  # 6. One speaker's LAYOUT position 80.6 mm off where the simulator really puts it (--sim-truth is the
  #    true layout): speaker 15, 1.39 m from the center, moved 40 mm toward the array and 70 mm across
  #    (2.9 deg of direction, which leave-one-out sees, and a 35 mm range error, which bends the center's
  #    trilateration). Measured: held out 4.3 us against a 3 us floor, the good speakers under 0.6 us.
  file(READ "${LAYOUT}" lj)
  string(JSON lj SET "${lj}" speakers 15 position "[1.46, 1.5, 0.07]")
  file(WRITE "${WORK}/moved.json" "${lj}")
  set(O --layout moved.json --sim-truth "${LAYOUT}" --simulate --zylia --mic 0.1 1.45 0.05 --sim-center 0.115,1.44,0.058)
  #    By default it refuses with exit 6, names speaker 15, writes nothing, and hands back the --speakers
  #    list without it
  run(outlier 6 ${O} --capsule-survey outlier.json)
  string(REGEX MATCHALL "FLAGGED: it does not fit the rest" fl "${outlier_OUT}")
  list(LENGTH fl nfl)
  if(NOT nfl EQUAL 1 OR NOT outlier_OUT MATCHES "\n  spk 15: held out +[0-9.]+ us, the rest [0-9.]+ us   FLAGGED")
    message(FATAL_ERROR "calibrate_cli_test: outlier: leave-one-out did not flag speaker 15 alone (log: ${WORK}/outlier.log)")
  endif()
  #    its 35 mm along the line is the range check's too, and nobody else's
  string(REGEX MATCHALL "RANGE FLAGGED" rfl "${outlier_OUT}")
  list(LENGTH rfl nrfl)
  if(NOT nrfl EQUAL 1 OR NOT outlier_OUT MATCHES "\n  spk 15: [0-9.]+ m, -3[0-9]\\.[0-9]+ mm, -3[0-9]\\.[0-9]+ from the median   RANGE FLAGGED")
    message(FATAL_ERROR "calibrate_cli_test: outlier: the range check did not flag speaker 15 alone (log: ${WORK}/outlier.log)")
  endif()
  expect(outlier "leave-one-out flagged speaker\\(s\\) 15 \\(" "the flagged speaker, named")
  file(READ "${WORK}/outlier.log" lg)
  if(NOT lg MATCHES "1 speaker\\(s\\) flagged, 15 \\(leave-one-out and range\\); the worst speaker 15: it does not"
     OR NOT lg MATCHES "--speakers 0-14,16-25, or pass --drop-outliers")
    message(FATAL_ERROR "calibrate_cli_test: outlier: the refusal does not name speaker 15 and the way out (log: ${WORK}/outlier.log)")
  endif()
  if(EXISTS "${WORK}/outlier.json")
    message(FATAL_ERROR "calibrate_cli_test: outlier: the refused survey was written")
  endif()
  #    --drop-outliers: speaker 15 dropped, everything solved again without it, and the result is the clean
  #    run's: the table within 0.1 mm of the truth, the acoustic center within 1 mm of the real one (a center
  #    still trilaterated with speaker 15's range lands 4.9 mm off while the table stays within 0.02 mm, so
  #    the center is the check that sees it), 25 observations in the file
  run(dropped 0 ${O} --capsule-survey dropped.json --drop-outliers)
  # the file first: the printed lines below can say "dropped" over a survey that kept the speaker
  file(READ "${WORK}/dropped.json" dj)
  string(JSON nobs GET "${dj}" observations)
  if(NOT nobs EQUAL 25)
    message(FATAL_ERROR "calibrate_cli_test: dropped: the file holds ${nobs} observations, not 25")
  endif()
  expect(dropped "--drop-outliers: dropped speaker 15; solving again over the other 25"
         "speaker 15 is dropped")
  expect(dropped "the survey leaves out speaker 15 \\(--drop-outliers\\); 25 speakers remain" "and named as left out")
  string(FIND "${dropped_OUT}" "dropped speaker 15;" at)
  string(SUBSTRING "${dropped_OUT}" ${at} -1 after)
  if(after MATCHES "\n  spk 15:")
    message(FATAL_ERROR "calibrate_cli_test: dropped: speaker 15 is still in the solve after the drop (log: ${WORK}/dropped.log)")
  endif()
  if(after MATCHES "FLAGGED" OR NOT after MATCHES "against the table the other 24 solve to")
    message(FATAL_ERROR "calibrate_cli_test: dropped: the re-check did not run clean over the 25 (log: ${WORK}/dropped.log)")
  endif()
  grab_caps(dropped "sim-truth: cap" dt)
  grab_caps(dropped "reload: cap" dg)
  caps_worst(dw dt dg)
  message(STATUS "capsule survey, one speaker dropped: worst capsule ${dw_i} off the truth by ${dw} um^2")
  if(dw GREATER 10000)
    message(FATAL_ERROR "calibrate_cli_test: dropped: the saved table is ${dw} um^2 off the truth at capsule ${dw_i}")
  endif()
  grab(dropped "capsule survey: acoustic center \\(${NUM} ${NUM} ${NUM}\\)" dc)
  dist2(d_dc dc sc)
  message(STATUS "capsule survey, one speaker dropped: acoustic center ${dc_s}, true ${sc_s}: ${d_dc} (0.1 mm)^2 off")
  if(d_dc GREATER 100)
    message(FATAL_ERROR "calibrate_cli_test: dropped: the acoustic center ${dc_s} is not where the array is, ${sc_s}")
  endif()
  #    A SECOND outlier: speaker 13 moved the same way. With 15 in the set leave-one-out misses it (held out
  #    4.4 us, but the rest's residual is 0.9 us) and the range check sees both (about 36 and 38 mm). Whichever
  #    is worse is dropped, the other still flags, and --drop-outliers stops there: exit 6, nothing written,
  #    both named, and neither in the --speakers list it hands back
  string(JSON lj SET "${lj}" speakers 13 position "[0.07, 1.5, 1.46]")
  file(WRITE "${WORK}/moved2.json" "${lj}")
  run(second 6 --layout moved2.json --sim-truth "${LAYOUT}" --simulate --zylia --mic 0.1 1.45 0.05
      --sim-center 0.115,1.44,0.058 --capsule-survey second.json --drop-outliers)
  file(READ "${WORK}/second.log" lg)
  if(NOT lg MATCHES "dropped speaker (1[35]), and the checks now flag\n +speaker (1[35]) \\(")
    message(FATAL_ERROR "calibrate_cli_test: second: the second outlier did not stop the run by name (log: ${WORK}/second.log)")
  endif()
  if(CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
    message(FATAL_ERROR "calibrate_cli_test: second: speaker ${CMAKE_MATCH_1} was dropped and then flagged again (log: ${WORK}/second.log)")
  endif()
  if(NOT lg MATCHES "--speakers 0-12,14,16-25")
    message(FATAL_ERROR "calibrate_cli_test: second: the --speakers list it hands back keeps an outlier (log: ${WORK}/second.log)")
  endif()

  # 7. The range check against the MEDIAN residual, not zero.
  #    A wrong --latency, 20 mm long, on a clean set (the cube's corners and face centers, around the array):
  #    every range residual carries it, so a residual held against ZERO would flag all 14. Against the median
  #    nobody is flagged, the survey is written, and the shared error is called out as the latency's.
  run(wronglat 0 --layout "${LAYOUT}" --simulate --zylia --mic 0.1 1.45 0.05 --sim-center 0.115,1.44,0.058
      --speakers 0,2,4,6,8,10,12,13,15,17,19,21,23,25 --latency 3.679 --capsule-survey wronglat.json)
  if(wronglat_OUT MATCHES "FLAGGED|flagged speaker")
    message(FATAL_ERROR "calibrate_cli_test: wronglat: a latency every speaker shares flagged a speaker (log: ${WORK}/wronglat.log)")
  endif()
  expect(wronglat "the median range residual is \\+(19|2[01])\\.[0-9]+ mm" "the median carries the 20 mm the latency is off by")
  expect(wronglat "WARNING every speaker's range, --latency taken off, reads about (19|2[01])\\.[0-9] mm shorter than its layout\n +distance: --latency is about that much \\([0-9]+ us\\) too large"
         "the shared error is named as the latency's")
  if(NOT EXISTS "${WORK}/wronglat.json")
    message(FATAL_ERROR "calibrate_cli_test: wronglat: the clean survey was not written")
  endif()

  #    The latency poorly pinned: ten speakers of the 24-speaker dome, all on its upper side at 2 m, whose
  #    dilution is over 3. Clean, the range check flags nobody (the median takes the latency's trade with the
  #    center out). With speaker 18 40 mm out radially it flags 18 as the worst (a good speaker can flag beside
  #    it: ten speakers against four unknowns bend more than 26 do), and --drop-outliers drops 18 and runs clean.
  file(READ "${CMAKE_CURRENT_LIST_DIR}/../examples/dome_24.json" dj0)
  file(WRITE "${WORK}/dome.json" "${dj0}")
  string(JSON dj0 SET "${dj0}" speakers 18 position "[1.1574, 2.7221, -1.1527]")
  file(WRITE "${WORK}/dome_r18.json" "${dj0}")
  set(D --sim-truth dome.json --simulate --zylia --mic 0 1.5 0 --sim-center 0.015,1.49,0.008 --speakers 14-23)
  run(dome_clean 0 --layout dome.json ${D} --capsule-survey dome_clean.json)
  expect(dome_clean "poorly determined from these speakers \\(dilution [3-9]\\.[0-9]" "the dome's upper side pins the latency poorly")
  if(dome_clean_OUT MATCHES "FLAGGED|flagged speaker" OR NOT dome_clean_OUT MATCHES "flagged over 10 mm off the median")
    message(FATAL_ERROR "calibrate_cli_test: dome_clean: the range check did not run clean with the latency poorly pinned (log: ${WORK}/dome_clean.log)")
  endif()
  run(dome_r18 0 --layout dome_r18.json ${D} --capsule-survey dome_r18_out.json --drop-outliers)
  expect(dome_r18 "\n  spk 18: [0-9.]+ m, \\+[0-9.]+ mm, \\+[0-9.]+ from the median   RANGE FLAGGED" "speaker 18's range is flagged")
  expect(dome_r18 "--drop-outliers: dropped speaker 18;" "speaker 18 is the worst, and dropped")
  string(FIND "${dome_r18_OUT}" "dropped speaker 18;" at)
  string(SUBSTRING "${dome_r18_OUT}" ${at} -1 after)
  if(after MATCHES "FLAGGED")
    message(FATAL_ERROR "calibrate_cli_test: dome_r18: something still flags once speaker 18 is gone (log: ${WORK}/dome_r18.log)")
  endif()
  expect(dome_r18 "poorly determined from these speakers \\(dilution [3-9]\\.[0-9]" "still the poorly pinned latency")
  if(EXISTS "${WORK}/second.json")
    message(FATAL_ERROR "calibrate_cli_test: second: a survey was written past a second outlier")
  endif()

  # 4. Refusals, each before any sweep, each writing nothing: too few speakers, a coplanar set (the middle
  #    ring, at the center's height), no --zylia, no --mic, an input --survey, stray knobs
  set(U --layout "${LAYOUT}" --simulate --zylia --mic 0 1.5 0.02)
  run(few 2 ${U} --capsule-survey few.json --speakers 0,4,21)
  file(READ "${WORK}/few.log" lg)
  if(NOT lg MATCHES "3 speaker\\(s\\); the survey needs 4 or more")
    message(FATAL_ERROR "calibrate_cli_test: few: three speakers were not refused by count (log: ${WORK}/few.log)")
  endif()
  run(flat 2 --layout "${LAYOUT}" --simulate --zylia --mic 0 1.5 0 --capsule-survey flat.json --speakers 9-16)
  file(READ "${WORK}/flat.log" lg)
  if(NOT lg MATCHES "the 8 speakers' directions from --mic have a spread of 0\\.000, under\n +the survey's 0\\.05 floor")
    message(FATAL_ERROR "calibrate_cli_test: flat: the middle ring was not refused as coplanar (log: ${WORK}/flat.log)")
  endif()
  run(nozylia 2 --layout "${LAYOUT}" --simulate --mic 0 1.5 0 --capsule-survey nz.json)
  run(nomic 2 --layout "${LAYOUT}" --simulate --zylia --capsule-survey nm.json)
  run(insurvey 2 ${U} --capsule-survey is.json --survey body.json)
  run(badlist 2 ${U} --capsule-survey bl.json --speakers 3,3)
  run(spknosurvey 2 ${U} --speakers 0-5)
  run(offnosim 2 ${U} --capsule-survey on.json --track-sim-offset 0,0,0)
  run(centertracked 2 ${U} --capsule-survey ct.json --track-sim --sim-center 0,1.5,0)
  run(dropnosurvey 2 ${U} --drop-outliers)
  # each stray knob is refused for ITS reason, not for whatever refusal came first
  foreach(pair "badlist|names speaker 3 twice" "spknosurvey|--speakers picks the --capsule-survey's speakers"
               "offnosim|--track-sim-offset is the SIMULATED stand's true offset" "centertracked|--sim-center is where"
               "insurvey|takes no --survey" "nomic|needs --mic x y z" "nozylia|pass --zylia"
               "dropnosurvey|--drop-outliers drops the speakers the --capsule-survey's leave-one-out flags")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 nm)
    list(GET pair 1 why)
    file(READ "${WORK}/${nm}.log" lg)
    string(FIND "${lg}" "${why}" at)
    if(at LESS 0)
      message(FATAL_ERROR "calibrate_cli_test: ${nm}: refused, but not with '${why}' (log: ${WORK}/${nm}.log)")
    endif()
  endforeach()
  foreach(f few flat nz nm is bl on ct)
    if(EXISTS "${WORK}/${f}.json")
      message(FATAL_ERROR "calibrate_cli_test: the refused run '${f}' wrote a survey")
    endif()
  endforeach()

else()
  message(FATAL_ERROR "calibrate_cli_test: unknown MODE '${MODE}'")
endif()
