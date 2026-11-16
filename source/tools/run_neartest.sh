#!/bin/bash
# Near-end test matrix: keyboard / mouse / breath / mouth / desk noises + voice + (simulated) echo, run through the
# production chain with the user's settings and scored per class (see tools/vf_synth.c).
#
#   bash tools/run_neartest.sh TAG ["set breath sens 0.7;bypass nn on"]      from source/, after building
#
# Needs build/looptest/floor_mic.wav + floor_ref.wav (vf-loop with a silent far end: the real microphone floor),
# build/looptest/ref_babble2.wav (a real loopback of a far-end voice) and build/audiotest/speech_pl.wav.
# Scenarios x SEEDS (default 4), cached in build/looptest/sc:
#   pauses   floor + near-end sounds, far end plays but does NOT reach the microphone (headset on the head)
#   echo     the same + a simulated echo path (delay 50 ms, -15 dB, speaker nonlinearity, 20 ppm drift, room tail)
#   quiet    floor + near-end sounds, nothing playing on the PC
#   talking  floor + speech + near-end sounds between the words, far end plays (headset)
# Output: build/looptest/res_TAG.csv and a table of means over the seeds.
set -e
TAG=${1:-run}; EXTRA=${2:-}
SEEDS=${SEEDS:-4}
cd "$(dirname "$0")/../build"          # relative paths from here: the project folder has a space in its name
B=.; L=looptest; SC=looptest/sc; mkdir -p "$SC"
S=./vf-synth.exe; F=./mic-thingy-file.exe
for f in "$L/floor_mic.wav" "$L/ref_babble2.wav" "$B/audiotest/speech_pl.wav"; do [ -f "$f" ] || { echo "missing $f"; exit 1; }; done
LEN=21
[ -f "$SC/voice.csv" ] || $S voicemap "$B/audiotest/speech_pl.wav" "$SC/voice.csv" >/dev/null
[ -f "$SC/echo.wav" ] || $S echo "$SC/echo.wav" "$L/ref_babble2.wav" --delay 50 --gain -15 --room 0.4 --nl 3 --drift 20 --lp 9000 >/dev/null

# the user's settings (CLAUDE.md): aec on, delay 40, strength 0.4, nn erbdf, gate 0.79, breath on
CMDS=(-c "bypass aec off" -c "set aec delay 40" -c "set aec strength 0.4" -c "bypass nn off" -c "set nn model 1" -c "set gate threshold 0.79" -c "bypass breath off")
IFS=';' read -ra X <<< "$EXTRA"
for c in "${X[@]}"; do [ -n "$c" ] && CMDS+=(-c "$c"); done

OUT="$L/res_$TAG.csv"; : > "$OUT"
for s in $(seq 1 $SEEDS); do
  # near-end sounds: dense for the pause scenarios, between the words for "talking"
  [ -f "$SC/near_$s.wav" ] || $S gen --len $LEN --seed $s --density 1.4 "$SC/near_$s.wav" "$SC/near_$s.csv" >/dev/null
  [ -f "$SC/nearv_$s.wav" ] || $S gen --len $LEN --seed $((s + 50)) --density 2.5 --avoid "$SC/voice.csv" "$SC/nearv_$s.wav" "$SC/nearv_$s.csv" >/dev/null
  for sc in pauses echo quiet talking; do
    mic="$SC/${sc}_$s.wav"; ev="$SC/near_$s.csv"; ref="$L/ref_babble2.wav"
    if [ ! -f "$mic" ]; then
      case $sc in
        pauses)  $S mix "$mic" "$L/floor_mic.wav" "$SC/near_$s.wav:0" ;;
        echo)    $S mix "$mic" "$L/floor_mic.wav" "$SC/near_$s.wav:0" "$SC/echo.wav:0" ;;
        quiet)   $S mix "$mic" "$L/floor_mic.wav" "$SC/near_$s.wav:0" ;;
        talking) $S mix "$mic" "$L/floor_mic.wav" "$SC/nearv_$s.wav:0" "$B/audiotest/speech_pl.wav:-4" ;;
      esac
    fi
    [ $sc = quiet ] && ref="$L/floor_ref.wav"
    if [ $sc = talking ]; then ev="$SC/ev_talk_$s.csv"; cat "$SC/nearv_$s.csv" "$SC/voice.csv" > "$ev"; fi
    $F -i "$mic" --ref "$ref" -o "$SC/out_${TAG}_${sc}_$s.wav" "${CMDS[@]}" 2>/dev/null
    $S score "$ev" "$mic" "$SC/out_${TAG}_${sc}_$s.wav" "$ref" | grep '^CSV' | sed "s/^CSV,/$sc,$s,/" >> "$OUT"
  done
done

echo "=== $TAG: means over $SEEDS seeds ${EXTRA:+($EXTRA)}"
echo "    energy / peak = dB change in the event windows (negative = removed), audible = % of events still audible"
awk -F, '
  { k = $1 "," $3; n[k]++; a[k] += $5; p[k] += $6; l[k] += $7; c[k] += $4; if (!(k in ord)) { ord[k] = ++m; key[m] = k } }
  END {
    cur = "";
    for (i = 1; i <= m; i++) {
      k = key[i]; split(k, q, ",");
      if (q[1] != cur) { cur = q[1]; printf("\n[%s]\n", cur) }
      if (q[2] == "voice")        printf("  %-8s level %+6.1f dB  peak %+6.1f dB  onset loss %5.1f dB\n", q[2], a[k]/n[k], p[k]/n[k], l[k]/n[k]);
      else if (q[2] == "outside") printf("  %-8s change %+6.1f dB  floor %6.1f dBFS  correlation with the reference %.3f\n", q[2], a[k]/n[k], p[k]/n[k], l[k]/n[k]);
      else                        printf("  %-8s %3d ev/seed  energy %+6.1f dB  peak %+6.1f dB  audible %5.1f %%\n", q[2], c[k]/n[k], a[k]/n[k], p[k]/n[k], l[k]/n[k]);
    }
  }' "$OUT"
