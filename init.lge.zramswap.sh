#!/vendor/bin/sh
#
# LG zram swap bring-up.
#
# Updated for the reworked zram stack in this kernel:
#   - the driver is the ACK android13-5.15 one with 6.6 recompression, so
#     the pre-5.x "async"/"max_write_threads" knobs no longer exist and
#     max_comp_streams is a compatibility stub
#   - the compressor is chosen through comp_algorithm and is now selectable
#     per device (lz4, lz4k, lz4kd, lz4k_oplus, zstd, zstdn, ...)
#   - secondary algorithms can be registered in priority order through
#     recomp_algorithm, and a pass is started with the recompress node
#   - low_compress_ratio reports how many pages compress badly (mm_stat
#     field 10), which is what tells us whether recompression is worth it
#
# The rest of the ported zram work needs no configuration: partial I/O (no
# 4K alignment requirement any more), vzalloc'ed buffers and the reschedule
# in the idle loop are internal. max_comp_streams is a 5.x compatibility
# stub, writeback needs CONFIG_ZRAM_WRITEBACK plus a backing file on /data
# and is not enabled. Recompression is deliberately driven from userspace:
# hook "init.lge.zramswap.sh recompress" to a periodic service, for example
#
#   service zram-recompress /vendor/bin/init.lge.zramswap.sh recompress
#       class late_start
#       user root
#       oneshot
#       disabled
#
# and start it from a timer or a screen-off/power-connected trigger.
#

target=`getprop ro.board.platform`
device=`getprop ro.product.device`
product=`getprop ro.product.name`

# --- runtime overrides ------------------------------------------------------
# Everything below can be overridden from a system property, so tuning a 4 GB
# device (or A/B testing an algorithm) needs a property change and a reboot
# instead of a new script:
#   persist.zram.algo          primary compressor             (default lz4k)
#   persist.zram.recomp        secondary list, space separated (default "lz4hc zstdn")
#   persist.zram.size          zram0 size in kB               (default: per target)
#   persist.zram.low_ratio     low compression ratio, percent   (default 75)
#   persist.zram.page_cluster  vm.page-cluster                 (default 0)
#   persist.zram.watermark     vm.watermark_scale_factor       (default: 100 on sdm845)
#   persist.zram.idle_only     1 = recompress idle pages only   (default 0)
#
prop_algo=`getprop persist.zram.algo`
prop_recomp=`getprop persist.zram.recomp`
prop_size=`getprop persist.zram.size`
prop_low_ratio=`getprop persist.zram.low_ratio`
prop_page_cluster=`getprop persist.zram.page_cluster`
prop_watermark=`getprop persist.zram.watermark`
prop_idle_only=`getprop persist.zram.idle_only`

# --- zram compressor configuration -----------------------------------------
# Primary compressor used for the swap device. Available ones can be read
# from /sys/block/zram0/comp_algorithm; lz4k/lz4kd/lz4k_oplus/zstdn are the
# Xiaomi-derived variants ported from the zram optimizations branch.
zram_algo=lz4k

# Secondary compressors tried, in order, when a page is recompressed. Only
# used when the kernel has CONFIG_ZRAM_MULTI_COMP and recomp_algorithm.
# lz4hc first: it is a high-compression LZ4 *encoder*, so pages recompressed
# with it still decode at plain lz4 speed, which keeps swap-in cheap. zstdn
# is the fallback for pages lz4hc cannot shrink; when every secondary
# algorithm fails the kernel marks the page incompressible and stops trying.
zram_recomp_algos="lz4hc zstdn"

# Pages whose compression saves less than this percentage are counted as
# "poorly compressed" and reported as the 10th field of mm_stat. 0 disables
# the accounting.
zram_low_ratio=75

# Set to 1 to mark all pages idle before a recompress pass, so that only
# pages that were not written recently are recompressed. This is a rough
# heuristic: the kernel has no other source of "this page is cold".
zram_recompress_idle_only=0

# vm.watermark_scale_factor applied below; 0 leaves the kernel default alone
# (the sdm845 branch raises it to 100).
watermark_scale=0

[ -n "${prop_algo}" ] && zram_algo=${prop_algo}
[ -n "${prop_recomp}" ] && zram_recomp_algos="${prop_recomp}"
[ -n "${prop_low_ratio}" ] && zram_low_ratio=${prop_low_ratio}
[ -n "${prop_idle_only}" ] && zram_recompress_idle_only=${prop_idle_only}

# --- helpers ---------------------------------------------------------------
set_zram_algo() {
  dev=$1
  # comp_algorithm lists "[current] available ...", so -w keeps "lz4" from
  # matching "lz4hc".
  if grep -qw "${zram_algo}" "${dev}/comp_algorithm" 2>/dev/null ; then
    echo "${zram_algo}" > "${dev}/comp_algorithm" && echo "zram algo ${zram_algo} on ${dev}"
  else
    echo "zram algo ${zram_algo} unavailable on ${dev}, keeping $(cat ${dev}/comp_algorithm)"
  fi
}

set_zram_recomp() {
  dev=$1
  [ -w "${dev}/recomp_algorithm" ] || return 0

  priority=1
  for alg in ${zram_recomp_algos}; do
    if grep -qw "${alg}" "${dev}/comp_algorithm" 2>/dev/null ; then
      echo "algo=${alg} priority=${priority}" > "${dev}/recomp_algorithm" \
        && echo "zram recomp ${alg} at priority ${priority} on ${dev}"
    fi
    priority=$((priority + 1))
  done

  if [ -w "${dev}/low_compress_ratio" ] ; then
    echo ${zram_low_ratio} > "${dev}/low_compress_ratio"
  fi
}

start() {
  # Check the available memory
  memtotal_str=$(grep 'MemTotal' /proc/meminfo)
  memtotal_tmp=${memtotal_str#MemTotal:}
  memtotal_kb=${memtotal_tmp%kB}

  echo MemTotal is $memtotal_kb kB

  # Check built-in zram devices
  nr_builtin_zram=$(ls /dev/block/zram* | grep -c zram)

  if [ "$nr_builtin_zram" -ne "0" ] ; then
    # Use the built-in zram devices
    nr_zramdev=${nr_builtin_zram}
    use_mod=0
  else
    use_mod=1
    # Detect the number of cores
    nr_cores=$(grep -c ^processor /proc/cpuinfo)

    # Evaluate the number of zram devices based on the number of cores
    nr_zramdev=${nr_cores/#0/1}
    echo The number of cores is $nr_cores
  fi
  echo zramdev $nr_zramdev

  # ZRAM tunable parameters
  nr_multi_zram=1
  sz_zram0=0
  zram_async=0
  swappiness_new=100

  case $target in
    "msm8937")
      nr_multi_zram=4
      zram_async=0
      max_write_threads=0
      if [ $nr_zramdev -gt 1 ] ; then
        sz_zram0=$(( memtotal_kb / 8 * 3 ))
        sz_zram=$(( memtotal_kb / 4 ))
      else
        if [ memtotal_kb -gt 2048000 ] ; then
          sz_zram=$(( memtotal_kb / 4 / ${nr_zramdev} ))
        else
          sz_zram=$(( memtotal_kb / 3 / ${nr_zramdev} ))
        fi
      fi
    ;;

    "msm8953")
      sz_zram=$(( memtotal_kb / 4 ))
      sz_zram0=$(( memtotal_kb / 4 ))
      nr_multi_zram=4
      if [ memtotal_kb -gt 2048000 ] ; then
        zram_async=1
        max_write_threads=4
      else
        zram_async=0
        max_write_threads=0
      fi
    ;;

    "msm8998" | "msm8996" | "msm8952")
      sz_zram=$(( memtotal_kb / 4 ))
      sz_zram0=$(( memtotal_kb / 4 ))
      nr_multi_zram=4
    ;;

    "sdm845" | "msmnile" | "sm6150")
      sz_zram=2621440
      sz_zram0=2621440
      nr_multi_zram=4
      zram_async=1
      max_write_threads=4
      # Start kswapd earlier so reclaim happens in the background instead of
      # stalling the faulting task (msmnile raises this to 200 below).
      watermark_scale=100
      
      if [ "$target" == "msmnile" ] ; then
        # Increase watermark about 2%
        echo 200 > /proc/sys/vm/watermark_scale_factor
      fi
    ;;

    "kona")
      if [ $memtotal_kb -gt 6291456 ] ; then
        sz_zram=3145728
      else
        sz_zram=$(( memtotal_kb / 2 ))
      fi
      nr_multi_zram=4
      echo 200 > /proc/sys/vm/watermark_scale_factor
      echo 0 > /proc/sys/vm/watermark_boost_factor
	;;

    "lito")
      sz_zram=$(( memtotal_kb / 2 ))
      nr_multi_zram=4
      echo 0 > /proc/sys/vm/watermark_boost_factor
      echo 200 > /proc/sys/vm/watermark_scale_factor
    ;;

    *)
      sz_zram=$(( memtotal_kb / 4 / ${nr_zramdev} ))
    ;;
  esac

  if [ -n "${prop_size}" ] ; then
    sz_zram0=${prop_size}
    sz_zram=${prop_size}
    echo "zram size overridden to ${prop_size} kB by persist.zram.size"
  fi

  echo sz_zram size is ${sz_zram}

  # Load kernel module for zram
  if [ "$use_mod" -eq "1"  ] ; then
    modpath=/system/lib/modules/zram.ko
    modargs="num_devices=${nr_zramdev}"
    echo zram.ko is $modargs

    if [ -f $modpath ] ; then
      insmod $modpath $modargs && (echo "zram module loaded") || (echo "module loading failed and exiting(${?})" ; exit $?)
    else
      echo "zram module not exist(${?})"
      exit $?
    fi
  fi

  # Initialize and configure the zram devices as a swap partition
  zramdev_num=0
  if [ "$sz_zram0" -eq "0" ] ; then
    sz_zram0=$((${sz_zram} * ${nr_zramdev}))
  fi
  while [[ $zramdev_num -lt $nr_zramdev ]]; do
    zramdev=/sys/block/zram${zramdev_num}

    # The compressor has to be chosen before the device is set up: changing
    # it later resets the device and would throw the swapped pages away.
    set_zram_algo ${zramdev}

    modpath_comp_streams=${zramdev}/max_comp_streams
    if [ -f $modpath_comp_streams ] ; then
      echo $nr_multi_zram > $modpath_comp_streams
    fi
    # async/max_write_threads were dropped from the driver in 4.20; they are
    # only written when a legacy kernel still exposes them.
    if [ -w ${zramdev}/async ] ; then
      echo $zram_async > ${zramdev}/async
    fi
    if [ -w ${zramdev}/max_write_threads ] ; then
      echo $max_write_threads > ${zramdev}/max_write_threads
    fi
    if [ "$zramdev_num" -ne "0" ] ; then
      echo ${sz_zram}k > ${zramdev}/disksize
    else
      echo ${sz_zram0}k > ${zramdev}/disksize
    fi

    # Secondary algorithms for recompression, in priority order.
    set_zram_recomp ${zramdev}

    mkswap /dev/block/zram${zramdev_num} && (echo "mkswap ${zramdev_num}") || (echo "mkswap ${zramdev_num} failed and exiting(${?})" ; exit $?)
    swapon /dev/block/zram${zramdev_num} && (echo "swapon ${zramdev_num}") || (echo "swapon ${zramdev_num} failed and exiting(${?})" ; exit $?)
    zramdev_num=$((zramdev_num + 1))
  done

  # Tweak VM parameters considering zram/swap
  deny_minfree_change=0
  overcommit_memory=1
  # zram swap wants little swap readahead: 3 clusters means every fault
  # decompresses 8 pages, and the extra ones are usually wasted on a
  # compressed device. 0 disables readahead, 1 reads two pages.
  page_cluster=0
  [ -n "${prop_page_cluster}" ] && page_cluster=${prop_page_cluster}
  if [ "$deny_minfree_change" -ne "1" ] ; then
	let min_free_kbytes=$(cat /proc/sys/vm/min_free_kbytes)*2
  fi
  laptop_mode=0

  echo $swappiness_new > /proc/sys/vm/swappiness
  echo $overcommit_memory > /proc/sys/vm/overcommit_memory
  echo $page_cluster > /proc/sys/vm/page-cluster
  if [ "$deny_minfree_change" -ne "1" ] ; then
	echo $min_free_kbytes > /proc/sys/vm/min_free_kbytes
  fi
  echo $laptop_mode > /proc/sys/vm/laptop_mode

  # Earlier background reclaim: fewer direct reclaim stalls on a 4 GB device.
  if [ -n "${prop_watermark}" ] ; then
    watermark_scale=${prop_watermark}
  fi
  if [ -n "${watermark_scale}" ] && [ "${watermark_scale}" -ne 0 ] ; then
    if echo ${watermark_scale} > /proc/sys/vm/watermark_scale_factor ; then
      echo "watermark_scale_factor=${watermark_scale}"
    fi
  fi
}

# Ask the kernel to recompress the pages already in zram with the secondary
# algorithms configured above. Called from a periodic job (init .rc service
# with a timer, cron, or by hand): the script itself does not loop.
recompress() {
  for zramdev in /sys/block/zram* ; do
    [ -w "${zramdev}/recompress" ] || continue
    [ "$(cat ${zramdev}/initstate 2>/dev/null)" = "1" ] || continue

    if [ "$zram_recompress_idle_only" -eq "1" ] ; then
      # Mark everything idle first, otherwise type=idle matches nothing.
      if [ -w "${zramdev}/idle" ] ; then
        echo all > "${zramdev}/idle"
      fi
      echo "type=idle" > "${zramdev}/recompress" && echo "recompressed idle pages on ${zramdev}"
    else
      # No type= means every allocated page; the kernel only keeps the result
      # when the secondary algorithm really produced a smaller object.
      echo "threshold=0" > "${zramdev}/recompress" && echo "recompressed ${zramdev}"
    fi

    # How many pages still compress below zram_low_ratio percent (mm_stat
    # field 10) - useful to decide whether another pass is worth it.
    if [ -r "${zramdev}/mm_stat" ] ; then
      echo "${zramdev} low-ratio pages: $(cat ${zramdev}/mm_stat | awk '{print $10}')"
    fi
  done
}

stop() {
  swaps=$(grep zram /proc/swaps)
  swaps=${swaps%%partition*}
  if [ $swaps ] ; then
    for i in $swaps; do
     swapoff $i
    done
    for j in $(ls /sys/block | grep zram); do
      echo 1 > /sys/block/${j}/reset
    done
    if [ $(lsmod | grep -c zram) -ne "0" ] ; then
      rmmod zram && (echo "zram unloaded") || (echo "zram unload fail(${?})" ; exit $?)
    fi
  fi
}

cmd=${1-start}

case $cmd in
  "start") start
  ;;
  "stop") stop
  ;;
  "recompress") recompress
  ;;
  *) echo "Undefined command!"
  ;;
esac
