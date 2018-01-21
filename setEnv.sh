#!/bin/bash
OCR_HOME=/home/berserker/Repository/OCR/xstg/ocr

APP_HOME=/home/berserker/Repository/OCR/xstg/apps

TOOL_HOME=/home/berserker/Repository/OCR/xstg/OCR-Racer

export OCR_INSTALL=$OCR_HOME/install
export LD_LIBRARY_PATH=$OCR_HOME/install/lib:$APP_HOME/libs/install/x86/lib:$LD_LIBRARY_PATH
export OCR_CONFIG=$APP_HOME/fib/install/x86/generated.cfg
export CONFIG_NUM_THREADS=8
export OCR_TYPE=x86
