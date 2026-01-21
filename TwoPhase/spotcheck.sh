#!/bin/bash
for val in "aborted" "working" "prepared"; 
    do 
    grep -o $val tlc_states.json | wc -l
    grep -o $val cpp_states.json | wc -l; 
done