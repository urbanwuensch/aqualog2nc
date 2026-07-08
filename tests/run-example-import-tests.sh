#!/usr/bin/env bash

set -euo pipefail

# Path to the converter
OPJ2DAT="${OPJ2DAT:-../build/opj2dat}"

TESTDIR="examples"

failed=0

for opj in "$TESTDIR"/*.opj; do
    data="${opj}.1.dat"
    data_orig="${opj}.1.dat.orig"
    output="${opj}.out"
    output_orig="${opj}.out.orig"

    printf "Testing %-20s ... \n" "$(basename "$opj")"

    rm -f "$data" "$output"

    "$OPJ2DAT" "$opj" > "${output}"

    #check output
    if diff -u "$output" "$output_orig"; then
        echo "OUTPUT PASS"
        rm -f "$output"
    else
        echo "OUTPUT FAIL"
        failed=1
    fi

    # check data
    # TODO: also .2, etc.
    if [ -e $data_orig ]; then
        if diff -u "$data" "$data_orig"; then
            echo "DATA PASS"
       	    rm -f "$data"
        else
            echo "DATA FAIL"
            failed=1
        fi
    fi
done

exit $failed
