
idf_component_register(SRCS
    main.cpp
    min_logger/min_logger.h
    min_logger/min_logger.cpp
    min_logger/min_logger_crc.h
    min_logger/min_logger_buffered_esp32.h
    min_logger/platform_implementations/defaults.cpp
    min_logger/platform_implementations/lock_free_ring_buffer.h
    min_logger/platform_implementations/lock_free_ring_buffer.cpp
    min_logger/platform_implementations/buffered_esp32.cpp
)
