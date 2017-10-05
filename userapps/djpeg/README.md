<!--
     Copyright 2017, Data61
     Commonwealth Scientific and Industrial Research Organisation (CSIRO)
     ABN 41 687 119 230.

     This software may be distributed and modified according to the terms of
     the BSD 2-Clause license. Note that NO WARRANTY is provided.
     See "LICENSE_BSD2.txt" for details.

     @TAG(DATA61_BSD)
-->
# About

This app provides the decode stage of the jpeg benchmark from the mibench suite. See the details in rumprun packages.

# Usage

Two output files are packed into the cookfs by rumprun. To run this benchmark use either of the following arguments;

    + `djpeg -dct int -ppm -outfile /tmp/output_large_decode.jpeg /data/jpeg/input_large.ppm`
    + `djpeg -dct int -ppm -outfile /tmp/output_small_decode.jpeg /data/jpeg/input_small.ppm`

for large or small workloads respectively
