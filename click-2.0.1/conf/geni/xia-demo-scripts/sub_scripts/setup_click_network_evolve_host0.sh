#!/bin/bash

cd ~/xia-core/click-2.0 
sudo killall -9 click 
sudo userlevel/click conf/geni/host0.click >& /dev/null &

