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

else()
  message(FATAL_ERROR "calibrate_cli_test: unknown MODE '${MODE}'")
endif()
