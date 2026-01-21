#!/bin/bash
for val in "\"ncs\"" "\"e1\"" "\"e2\""; 
    do 
    grep -o $val bakery_states.json | wc -l
    grep -o $val tlc_states.json | wc -l; 
done