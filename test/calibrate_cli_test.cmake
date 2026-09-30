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
#   live_zylia    --live N --zylia --simulate: a speaker moved 10 cm reads back as moved; a speaker
#                 turned 25 deg off reads about 25 deg against a reference speaker and worse from the
#                 file behind a simulated screen; the tilt peaks where the box points at the mic.
#   track_settle  --zylia --trims --track-sim --mount-offset x,y,z: waits (HOLD, then OK), takes the measured
#                 center, which is the simulated stand's TRUE center and not the target, and solves there.
#   track_ring    the same with --mount-offset ring (the simulated MODELDEF's uneven marker ring).
#   track_bump    the stand knocked 15 mm mid-run: exit 4, the bump message, nothing written.
#   track_refuse  the refusals: the tracked --zylia survey without a body-frame survey (and with a room-axes
#                 one), the live position readout without one (the tilt meter still runs), stray flags.
#   track_survey  a body-frame survey (offset + table): the tracked position survey lands every speaker.
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
  string(REGEX REPLACE "^0+([0-9])" "\\1" fp "${fp}")
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
  string(REGEX REPLACE "^0+([0-9])" "\\1" fp "${fp}")
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

  # aim: turned 25 deg off, then 12, then on axis, then back out. The reference is speaker 16, which
  # points at the mic; a 3 dB screen shelf (above 4 kHz) sits on every path, so the file's 0 deg tilt
  # is wrong by the screen's tilt and the reference's is not. 3 dB, not 1: the live bands (3-10 kHz
  # against 10 kHz up) both sit mostly above the shelf's corner, so a 1 dB shelf tilts them too
  # little (the file read 26.3 against the reference's 24.4) to test the point
  run(aim 0 ${LIVE} --sim-screen 3 --aim-ref 16 --sim-aim-steps 25,12,0,12,25)
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
  expect(aim "\n  #3 [^\n]*below 0\\.00 dB[^\n]*off-axis ref on axis \\(under [0-9]+ deg\\)[^\n]*true 0\\.0 deg\n"
         "on axis, the meter sits at its peak and the reference reads 'on axis'")
  expect(aim "\n  #4 [^\n]*below 0\\.[0-9][0-9] dB" "turned back out, the meter drops")
  expect(aim "live: peak tilt [-+][0-9.]+ dB at reading 3, where the true axis was 0\\.0 deg off the mic"
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

else()
  message(FATAL_ERROR "calibrate_cli_test: unknown MODE '${MODE}'")
endif()
