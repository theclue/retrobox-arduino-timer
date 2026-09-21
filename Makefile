# Prerequisiti: installa arduino-cli, poi esegui `make setup`.
# Uso rapido: `make compile`, `make upload` e `make monitor`.
# Per scegliere manualmente la porta: `make upload PORT=/dev/xxx`.

ARDUINO_CLI ?= arduino-cli
SKETCH := relay-timer
FQBN ?= arduino:avr:uno
BUILD_DIR := build
BAUD ?= 115200
UNAME := $(shell uname)

ifeq ($(UNAME),Darwin)
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem* /dev/cu.usbserial*))
else ifeq ($(UNAME),Linux)
PORT ?= $(firstword $(wildcard /dev/ttyACM* /dev/ttyUSB*))
else
PORT ?=
endif

.PHONY: help check-cli compile all upload monitor clean lib-install setup

help:
	@printf '%s\n' \
		'Target disponibili:' \
		'  help        Mostra questo aiuto' \
		'  check-cli   Verifica la presenza di arduino-cli' \
		'  compile     Compila lo sketch' \
		'  all         Alias di compile' \
		'  upload      Compila e carica lo sketch sulla scheda' \
		'  monitor     Avvia il monitor seriale' \
		'  clean       Rimuove i file di build' \
		'  lib-install  Installa le librerie richieste' \
		'  setup       Configura il core AVR e installa le librerie'

check-cli:
	@if ! command -v $(ARDUINO_CLI) >/dev/null 2>&1; then \
		echo 'Errore: arduino-cli non è installato o non è nel PATH.' >&2; \
		echo 'Su macOS puoi installarlo con: brew install arduino-cli' >&2; \
		echo 'Consulta anche: https://arduino.github.io/arduino-cli' >&2; \
		exit 1; \
	fi

compile: check-cli
	$(ARDUINO_CLI) compile --fqbn "$(FQBN)" --warnings all --build-path $(BUILD_DIR) $(SKETCH)

all: compile

upload: compile
	@if [ -z "$(PORT)" ]; then \
		 echo 'Errore: Nessuna porta rilevata. Specifica con: make upload PORT=/dev/...' >&2; \
		exit 1; \
	fi
	@case "$(PORT)" in /dev/*) ;; *) echo "PORT non valida: $(PORT)"; exit 1;; esac
	$(ARDUINO_CLI) compile --fqbn "$(FQBN)" --warnings all --build-path $(BUILD_DIR) --upload -p "$(PORT)" $(SKETCH)

monitor: check-cli
	@if [ -z "$(PORT)" ]; then \
		 echo 'Errore: Nessuna porta rilevata. Specifica con: make monitor PORT=/dev/...' >&2; \
		exit 1; \
	fi
	@case "$(PORT)" in /dev/*) ;; *) echo "PORT non valida: $(PORT)"; exit 1;; esac
	$(ARDUINO_CLI) monitor -p "$(PORT)" --config baudrate="$(BAUD)"

clean:
	rm -rf $(BUILD_DIR)

# Versioni pinnate in CI: vedi .github/workflows/ci.yml
lib-install: check-cli
	$(ARDUINO_CLI) lib install "LiquidCrystal I2C" "TimerOne" "PushButton" "Bounce2"

setup: check-cli
	$(ARDUINO_CLI) core update-index
	$(ARDUINO_CLI) core install arduino:avr
	$(MAKE) --no-print-directory lib-install
