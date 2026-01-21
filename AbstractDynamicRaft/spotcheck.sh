#!/bin/bash
for val in "Secondary" "Primary"; 
    do 
    grep -o $val states_tlc.json | wc -l
    grep -o $val states_cpp.json | wc -l; 
done