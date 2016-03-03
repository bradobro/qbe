#!/bin/sh

for t in test/*
do
	printf "Test $t ... "

	./lisc   $t >/tmp/out.0 2>&1
	./lisc.1 $t >/tmp/out.1 2>&1

	if diff /tmp/out.0 /tmp/out.1 > /dev/null
	then
		echo "OK"
	else
		echo "KO"
		break
	fi
done