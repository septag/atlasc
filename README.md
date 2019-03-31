[![Build Status](https://travis-ci.org/septag/atlasc.svg?branch=master)](https://travis-ci.org/septag/atlasc)

## Atlasc
[@septag](https://twitter.com/septagh)  

_atlasc_ is a command-line program that builds atlas textures from a bunch of input images.  
It can also be compiled as a static library which you can use it in your own toolsets.

## Features
- Can be compiled as a command-line tool and static library
- Light and fast
- Outputs atlas description to human-readable _json_ format. Generated images are _png_.
- Outputs both atlas description and image to native IFF binary file format.
- Supports zero-alpha trimming
- Supports polygon sprites

## Build
It has built and tested on the following platforms:

- __Windows__: Tested on Windows10 with visual studio 14 2015 update 3 (Win64).  
- __Linux__: Tested on ubuntu 16 with clang (6.0.0) and gcc (7.3.0). Package requirements:  
- __MacOS__: Tested on MacOS High Sierra - AppleClang 9.1.0

#### CMake options
- **STATIC_LIB**: instead of command-line, generates static-library. see `include/atlasc` to review the API.

## Usage

## Open-Source libraries used
- [sx](https://github.com/septag/sx): Portable base library
- [sjson](https://github.com/septag/sjson): Fast and portable single-header C file Json encoder/decoder
- [stb](https://github.com/nothings/stb): stb single-file public domain libraries for C/C++
- [sproutline](https://github.com/ands/sproutline): A small single-file library for sprite outline extraction and simplification
- [delanuay](https://github.com/eloraiby/delaunay): Relatively Robust Divide and Conquer 2D Delaunay Construction Algorithm


[License (BSD 2-clause)](https://github.com/septag/atlasc/blob/master/LICENSE)
--------------------------------------------------------------------------

<a href="http://opensource.org/licenses/BSD-2-Clause" target="_blank">
<img align="right" src="http://opensource.org/trademarks/opensource/OSI-Approved-License-100x137.png">
</a>

	Copyright 2019 Sepehr Taghdisian. All rights reserved.
	
	https://github.com/septag/atlasc
	
	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:
	
	   1. Redistributions of source code must retain the above copyright notice,
	      this list of conditions and the following disclaimer.
	
	   2. Redistributions in binary form must reproduce the above copyright notice,
	      this list of conditions and the following disclaimer in the documentation
	      and/or other materials provided with the distribution.
	
	THIS SOFTWARE IS PROVIDED BY COPYRIGHT HOLDER ``AS IS'' AND ANY EXPRESS OR
	IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
	MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
	EVENT SHALL COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
	INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
	BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
	DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
	LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
	OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
	ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
