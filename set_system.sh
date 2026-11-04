#!/bin/bash

emerson ctl broker tty0 $'timescale 3600\r'
emerson ctl action /board/battery set_aging_factor 3600
emerson ctl broker tty0 $'radio wakeup 0\r'
emerson ctl action /patient/heart set_av_block third
emerson ctl action /patient/heart set_escape_rate 0
emerson ctl action /board/battery set_depletion 86.5