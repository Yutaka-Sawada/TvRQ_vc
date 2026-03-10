# Test vector RaptorQ for Visual C

TvRQ is a test vector implementation of RaptorQ. 
It's used to generate test data for compliance (correct encoding and decoding). 
This isn't implemented to be fast, and in fact it is a quite slow implementation of RaptorQ.

Thanks [BitRipple](https://www.bitripple.com) for putting test set.

# Usable on Microsoft Visual Studio

This is a simplified version of [TvRQ](https://github.com/lorinder/TvRQ). 
Because the original TvRQ cannot be compiled on Microsoft Visual Studio, 
I changed some points for compatibility with Microsoft Visual C.

# Notes

I don't include testing tools nor Python module. 
If you are interested in other tests, please refer [full set version](https://github.com/lorinder/TvRQ).
