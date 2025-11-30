PIO_PATH = /home/dave/.platformio/penv/bin/platformio

# Build everything
all:
	$(PIO_PATH) run

# Build specific environments
build-tracker:
	$(PIO_PATH) run -e tracker

build-gateway:
	$(PIO_PATH) run -e gateway

# Upload
upload-tracker:
	$(PIO_PATH) run -e tracker -t upload

upload-gateway:
	$(PIO_PATH) run -e gateway -t upload

# Monitor (will ask which port if multiple match, or specify port with PORT=/dev/ttyUSB0)
monitor:
	$(PIO_PATH) device monitor

# Clean
clean:
	$(PIO_PATH) run -t clean

# Install libraries (useful if pio run doesn't auto-install for some reason)
init:
	$(PIO_PATH) pkg install

