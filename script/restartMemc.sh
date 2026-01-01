#!/bin/bash

addr=$(head -1 ../memcached.conf)
port=$(awk 'NR==2{print}' ../memcached.conf)

# # kill old me
# ssh ${addr} -o StrictHostKeyChecking=no "cat /tmp/memcached.pid | xargs kill"

# # launch memcached
# ssh ${addr} -o StrictHostKeyChecking=no "memcached -u root -l ${addr} -p  ${port} -c 10000 -d -P /tmp/memcached.pid"
# sleep 1

# init 

# echo "addr: $addr, port: $port"

(
  sleep 1
  echo "flush_all"
  echo "set ServerNum 0 0 1"
  echo "0"
  echo "set ClientNum 0 0 1"
  echo "0"
  echo "quit"
) | telnet $addr $port


