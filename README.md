# Billion Row Challenge study/attempt

A whole bunch of reading online led to the current state of the project, runs sub 0.3 second times consistently on a fairly good machine

# How to run

- Make sure you're on linux or running WSL
- grab "creature_measurements.py" from the official 1 billion row challenge repo
- grab the needed "data/weather_stations.csv"
- generate the measurements.txt
- run `run.sh` - it expects number of threads and number of cores as arguments, it does fetch the needed data by default

```bash
. run.sh
```