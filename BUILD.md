## Legacy Build

cd ~/Developer/esp/v4.4.8/esp-idf
pyenv local 3.10.20
# ./install.sh
# ./install.sh esp32
# ~/.espressif/python_env/idf4.4_py3.10_env/bin/python -m pip install esptool
cd ..
cd intex-swg-iot
idf.py build
# ~/.espressif/python_env/idf4.4_py3.10_env/bin/python ../esp-idf/components/esptool_py/esptool/esptool.py
# ~/.espressif/python_env/idf4.4_py3.10_env/bin/python ../esp-idf/components/esptool_py/esptool/esptool.py chip-id
~/.espressif/python_env/idf4.4_py3.10_env/bin/python ../esp-idf/components/esptool_py/esptool/esptool.py -p /dev/cu.SLAB_USBtoUART -b 460800 --before default_reset --after hard_reset --chip esp32  write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0xd000 build/ota_data_initial.bin 0x20000 build/IntexSWG.bin
