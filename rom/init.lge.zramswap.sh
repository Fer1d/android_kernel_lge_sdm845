#!/vendor/bin/sh
#
# zram swap bring-up for the LG sdm845 (4 GB) device.
#
# Written against this kernel: ACK android13-5.15 zram driver, 6.6
# recompression, the LZ4K/Zstdn algorithms ported from the zram
# optimizations branch, and kernel/sysctl.c fixed so that
# vm.watermark_scale_factor is writable again.
#
# Nodes the driver actually offers (drivers/block/zram/zram_drv.c):
#   comp_algorithm      primary compressor; must be written before disksize
#   recomp_algorithm    secondary compressors, as "algo=X priority=N"
#   recompress          "type=idle" or "threshold=N" starts a pass
#   low_compress_ratio  percentage below which a page counts as badly packed
#   idle/idle_stat/new_stat   idle marking and idle generation counters
#   disksize, mm_stat, initstate, reset
# The pre-5.x async/max_write_threads nodes are gone, and max_comp_streams is
# only a compatibility stub (streams became per-cpu in 5.x), so neither is
# written any more. zram is built in, so there is no module to load.
#
# Tune from a property and reboot - no need to repack the script:
#   persist.zram.algo          primary compressor          (default lz4k)
#   persist.zram.recomp        secondary list              (default "lz4hc zstdn")
#   persist.zram.size          zram size in kB             (default 2621440)
#   persist.zram.low_ratio     low compression ratio, %     (default 75)
#   persist.zram.page_cluster  vm.page-cluster              (default 0)
#   persist.zram.watermark     vm.watermark_scale_factor    (default 100)
#   persist.zram.idle_only     1 = recompress idle pages     (default 0)
#   persist.zram.recomp_interval  seconds between passes, 0 = never (21600)
#   persist.zram.recomp_delay     seconds before the first pass     (300)
#
# Recompression is userspace driven on purpose: the driver only runs a pass
# when "recompress" is written, so something in userspace has to ask for it.
# init.lge.svelte.rc starts "recompress-loop", which is this script sleeping
# between passes. A pass is cheap to repeat: the kernel flags a page that no
# secondary algorithm could shrink as INCOMPRESSIBLE and never tries it again,
# so every pass only pays for the pages written since the previous one.
#

target=`getprop ro.board.platform`
[ "$target" = "sdm845" ] || echo "zramswap: unexpected platform '$target', continuing anyway"

# --- defaults ---------------------------------------------------------------
# lz4k is the primary: cheapest to write, and the LZ4K family is what the
# Xiaomi kernels use. lz4hc is the first secondary because it is a
# high-compression LZ4 *encoder*, so those pages still decode at lz4 speed and
# swap-in stays cheap; zstdn is only tried when lz4hc cannot shrink a page.
zram_algo=lz4k
zram_recomp_algos="lz4hc zstdn"
zram_size_kb=2621440
zram_low_ratio=75
zram_page_cluster=0
zram_watermark=100
zram_idle_only=0
zram_recomp_interval=21600
zram_recomp_delay=300

zram_swappiness=100
zram_overcommit=1
zram_laptop_mode=0

# --- property overrides -----------------------------------------------------
o=`getprop persist.zram.algo`          ; [ -n "$o" ] && zram_algo=$o
o=`getprop persist.zram.recomp`        ; [ -n "$o" ] && zram_recomp_algos="$o"
o=`getprop persist.zram.size`          ; [ -n "$o" ] && zram_size_kb=$o
o=`getprop persist.zram.low_ratio`     ; [ -n "$o" ] && zram_low_ratio=$o
o=`getprop persist.zram.page_cluster`  ; [ -n "$o" ] && zram_page_cluster=$o
o=`getprop persist.zram.watermark`     ; [ -n "$o" ] && zram_watermark=$o
o=`getprop persist.zram.idle_only`     ; [ -n "$o" ] && zram_idle_only=$o
o=`getprop persist.zram.recomp_interval`; [ -n "$o" ] && zram_recomp_interval=$o
o=`getprop persist.zram.recomp_delay`   ; [ -n "$o" ] && zram_recomp_delay=$o

# --- helpers ----------------------------------------------------------------
write_if() {  # write_if <file> <value> <label>
  if [ -w "$1" ] && echo "$2" > "$1" 2>/dev/null ; then
    echo "zramswap: $3 = $2"
  else
    echo "zramswap: $3 FAILED ($1 <- $2)"
    return 1
  fi
}

num_or() {  # num_or <value> <fallback>: properties are as typed as whoever set them
  case "$1" in
    ''|*[!0-9]*) echo "$2" ;;
    *)           echo "$1" ;;
  esac
}

setup_zram() {  # setup_zram <sysfs dir> <device node>
  dev=$1
  node=$2

  # Compressor first: changing it once the device is initialized resets the
  # device and would throw away the pages already swapped out.
  if grep -qw "${zram_algo}" "${dev}/comp_algorithm" 2>/dev/null ; then
    write_if "${dev}/comp_algorithm" "${zram_algo}" "${dev} algo"
  else
    echo "zramswap: ${zram_algo} not offered, keeping $(cat ${dev}/comp_algorithm)"
  fi

  write_if "${dev}/disksize" "${zram_size_kb}k" "${dev} disksize"

  # Secondary compressors, in the order the kernel will try them.
  prio=1
  for alg in ${zram_recomp_algos} ; do
    if grep -qw "${alg}" "${dev}/comp_algorithm" 2>/dev/null ; then
      write_if "${dev}/recomp_algorithm" "algo=${alg} priority=${prio}" \
               "${dev} recomp ${alg} (priority ${prio})"
    fi
    prio=$((prio + 1))
  done

  [ -w "${dev}/low_compress_ratio" ] &&
    write_if "${dev}/low_compress_ratio" "${zram_low_ratio}" "${dev} low_compress_ratio"

  mkswap "${node}" || { echo "zramswap: mkswap ${node} FAILED" ; return 1 ; }
  echo "zramswap: mkswap ${node}"
  swapon "${node}" || { echo "zramswap: swapon ${node} FAILED" ; return 1 ; }
  echo "zramswap: swapon ${node}"
}

start() {
  if [ ! -e /dev/block/zram0 ] ; then
    echo "zramswap: /dev/block/zram0 is missing, is CONFIG_ZRAM=y?"
    return 1
  fi

  # The driver pre-creates num_devices (one by default); use whatever is there.
  for node in /dev/block/zram* ; do
    setup_zram "/sys/block/${node##*/}" "${node}" || return 1
  done

  # VM tuning for a zram-backed swap on a 4 GB device
  write_if /proc/sys/vm/swappiness "${zram_swappiness}" "swappiness"
  write_if /proc/sys/vm/overcommit_memory "${zram_overcommit}" "overcommit_memory"
  write_if /proc/sys/vm/page-cluster "${zram_page_cluster}" "page-cluster"
  write_if /proc/sys/vm/laptop_mode "${zram_laptop_mode}" "laptop_mode"

  # kswapd reclaims earlier (100 = 1% of the managed pages of a zone).
  [ "${zram_watermark}" -ne 0 ] &&
    write_if /proc/sys/vm/watermark_scale_factor "${zram_watermark}" "watermark_scale_factor"

  # LG doubles the stock value to give kswapd room.
  mfk=$(( $(cat /proc/sys/vm/min_free_kbytes) * 2 ))
  write_if /proc/sys/vm/min_free_kbytes "${mfk}" "min_free_kbytes"

  cat /proc/swaps
}

# One recompression pass over every initialized device. Call it from a timer or
# on screen-off: it is CPU work, not something to spin on.
recompress() {
  for dev in /sys/block/zram* ; do
    [ -w "${dev}/recompress" ] || continue
    [ "$(cat ${dev}/initstate 2>/dev/null)" = "1" ] || continue

    if [ "${zram_idle_only}" -eq 1 ] ; then
      # type=idle only matches pages that were marked idle first.
      [ -w "${dev}/idle" ] && echo all > "${dev}/idle"
      write_if "${dev}/recompress" "type=idle" "${dev} recompress (idle pages)"
    else
      # No type= means every allocated page; the kernel keeps the result only
      # when a secondary algorithm really produced a smaller object.
      write_if "${dev}/recompress" "threshold=0" "${dev} recompress (all pages)"
    fi

    # mm_stat: 1 orig_size 2 compr_data_size 3 mem_used ... 10 low_ratio_pages
    [ -r "${dev}/mm_stat" ] && awk -v d="${dev}" '{
      printf "%s: orig=%.2fG compr=%.2fG ratio=%.1f%% mem_used=%.2fG low_ratio=%s\n",
             d, $1/1073741824, $2/1073741824, $2/$1*100, $3/1073741824, $10
    }' "${dev}/mm_stat"
  done
}

# Resident timer around recompress(). One sleeping shell is cheaper than any
# userspace daemon, and sleep does not wake the CPU while it waits. Set
# persist.zram.recomp_interval to 0 to turn the loop off without repacking.
recompress_loop() {
  # A bad property value must not reach sleep: a failing sleep would turn the
  # loop into a spin that recompresses in a tight circle.
  zram_recomp_interval=`num_or "${zram_recomp_interval}" 21600`
  zram_recomp_delay=`num_or "${zram_recomp_delay}" 300`

  if [ "${zram_recomp_interval}" -le 0 ] ; then
    echo "zramswap: recompress loop disabled (persist.zram.recomp_interval=0)"
    return 0
  fi

  # The first pass waits for the boot rush (and for the pages to pile up);
  # there is nothing to shrink while the system is still starting.
  echo "zramswap: recompress loop armed (first pass in ${zram_recomp_delay}s, then every ${zram_recomp_interval}s)"
  sleep "${zram_recomp_delay}"

  while : ; do
    recompress
    sleep "${zram_recomp_interval}"
  done
}

stop() {
  for node in $(grep zram /proc/swaps | awk '{print $1}') ; do
    swapoff "${node}" && echo "zramswap: swapoff ${node}"
  done
  for dev in /sys/block/zram* ; do
    [ -w "${dev}/reset" ] && echo 1 > "${dev}/reset"
  done
}

case "${1-start}" in
  start)           start ;;
  stop)            stop ;;
  recompress)      recompress ;;
  recompress-loop) recompress_loop ;;
  *)               echo "usage: $0 {start|stop|recompress|recompress-loop}" ;;
esac
