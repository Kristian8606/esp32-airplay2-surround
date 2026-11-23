# Included by both projects after project(): a 16 MB flash with the 8 MB
# partition table still builds and runs, but leaves 8 MB unused.
# partitions_16mb.csv uses it; it reaches a board only over USB
# (idf.py flash), not by an update over Wi-Fi.
if(CONFIG_ESPTOOLPY_FLASHSIZE_16MB AND
   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME MATCHES "(^|/)partitions\\.csv$")
    message(WARNING
        "Flash size is 16 MB, but the partition table is "
        "'${CONFIG_PARTITION_TABLE_CUSTOM_FILENAME}' (the 8 MB layout). "
        "To use the rest, set menuconfig -> Partition Table -> Custom "
        "partition CSV file to partitions_16mb.csv (../partitions_16mb.csv "
        "for a speaker) and flash the board once over USB.")
endif()
