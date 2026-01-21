
## Generate Optimized C++ version of TLA+ Spec

Take the chosen TLA+ spec (ask the user for which one) and generate a C++ program that generates its full reachable state space as the model checker would do but in a way as optimized as possible for a C++ implementation. Do this single threaded, and check with the user for how to instantiate the constant finite parameters in the compiled C++ version. Ensure that the C++ version dumps out all states in a standard JSON format, and can output this to a JSON file. Assume a general JSON dump format that contains a state array like `{ "states": [ {fp: <uint64_t>, val: <JSON>, initial: <boolean>}, ... ]}`, where 'val' is the actual JSON representation of that state, and 'fp' is some hash/fingerprint for that state. Also add an option to run this state space exploration with JSON dumping disabled.

Finally generate a Makefile with a simple, barebones default target for building it.

## Validate Conformance between TLC and C++ version 

Now validate to make sure that the set of states generated and dumped into JSON by the C++ version match the set of states generated and dumped by TLC in JSON. Generate a Python script that runs TLC to generate the same state space and dump it to JSON using the tla2tools-checkall.jar binary which supports a `-dump json states.json` argument, and then have the script validate that the states match between the TLC output and the C++ generated state space. 

Generate a simple validation report in Markdown after completing this.

## Benchmark Throughput Difference

Measure the throughput (states/second) difference in the state space generation states between TLC and the C++ version. Check with the user for the finite model config parameters to use for this run, and update the generated C++ version of the spec to account for this if needed. You can do this benchmark by measuring the total runtime of TLC for an exhaustive run, and measuring its time duration and from this compute distinct states per second, and doing this similarly for the C++ version. When doing this, disable JSON dumping for both TLC and C++ to avoid the associated overhead. In order to measure the throughput of TLC, make sure to use the time duration reported by the final output of TLC. You don't need to do multiple runs of each, a single run is fine.

Generate a simple markdown report file on the results once the benchmark is complete.