# alphahttpd

αhttpd is a fast and small HTTP/1.1 server.

Contents:

* [Features](#features)
* [Build & Run](#build---run)
* [Benchmark](#benchmark)
* [HTTP server as a library](#http-server-as-a-library)


## Features

Current features and limitations:

* Runs on Linux, FreeBSD, Windows (uses epoll, kqueue, IOCP)
* Multi-threaded, uses all CPUs by default
* Completely asynchronous file I/O (offload syscalls to other threads)
* Can work in active polling mode, improving overall performance
* HTTP/1.1 only
* Serves the file tree in `www/` directory by default
* Uses `index.html` as index file
* Generates index document (directory contents)
* Doesn't use sendfile()
* No caching
* No ETag, If-None-Match, Range
* stdout/stderr logging only
* SSE-optimized HTTP parser
* Basic command-line parameters; no configuration file
* Instant build and startup
* <100KB portable binary file
* <5000 lines of code (excluding dependencies)


## Build & Run

Build on Linux:

	git clone https://github.com/stsaz/ffbase
	git clone https://github.com/stsaz/ffos
	git clone https://github.com/stsaz/alphahttpd
	cd alphahttpd
	make -j4

Build on Linux for Windows:

	mingw64-make -j4 OS=windows

Build on FreeBSD:

	gmake -j4

Run:

	cd alphahttpd-0
	./alphahttpd

Use custom address and port:

	./alphahttpd -l 127.0.0.1:8080


## Benchmark

Performance results achieved with `aggressor` tool called like this:

	aggressor 127.0.0.1:8080/index.html -t 2 -c 2000 -n 700000

which requests a file with 22B data.

	Server       | RPS     | Configuration
	=============+=========+==============================
	αhttpd v0.3  | 156krps | -t 5 -T 2 2>/dev/null
	             | 163krps | -t 3 -T 3 -p 2>/dev/null
	             |  64krps | -t 1 -T 1 2>/dev/null
	             |  91krps | -t 1 -T 1 -p 2>/dev/null
	-------------+---------+------------------------------
	αhttpd v0.2  | 156krps | -t 7 2>/dev/null
	             |  59krps | -t 1 2>/dev/null
	-------------+---------+------------------------------
	nginx/1.17.9 | 153krps | worker_processes 7
	             |         | worker_rlimit_nofile 20000
	             |         | sendfile off
	             |  54krps | worker_processes 1

Notes:

* doesn't prove anything, just to keep track on performance
* nginx writes access log to a real file
* nginx has fd cache
* nginx sends 3 more response header fields (Date, ETag, Accept-Ranges)


## HTTP server as a library

αhttpd's program interface allows you to run αhttpd from your own project and configure everything, even choosing the filters that process HTTP requests.
You may also place your own filters into the HTTP request processing conveyor.

Pseudo code for starting alphahttpd with 'virtspace' plugin:

```C
	#include <alphahttpd.h>

	#include <http/{FILTER}.h>
	// ...
	static const struct alphahttpd_filter* ah_filters[] = {
		&alphahttpd_filter_{FILTER},
		// ...
		NULL
	};

	struct alphahttpd_conf ac;
	alphahttpd_conf(NULL, &ac);
	// ac.option = ...;

	static const struct alphahttpd_handler handlers[] = {
		{ "/", "GET", root_handler },
		{}
	};
	alphahttpd_filter_virtspace_init(&ac, handlers);

	ac.filters = ah_filters;

	alphahttpd *a = alphahttpd_new();
	alphahttpd_conf(a, ac);
	alphahttpd_run(a);
```


## Homepage

https://github.com/stsaz/alphahttpd
