#!/usr/bin/env bash
set -e

nc -kluvw 1 192.168.1.192 3333 | \
uv --project min-logger/python run min-logger-parser .pio/build/esp32_pio_min_logger.json \
  --log_format BINARY
