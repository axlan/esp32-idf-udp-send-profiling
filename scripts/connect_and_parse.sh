#!/usr/bin/env bash
set -e

nc -kluvw 1 0.0.0.0 3333 | \
uv --project min-logger/python run min-logger-parser .pio/build/esp32_pio_min_logger.json \
  --log_format BINARY
