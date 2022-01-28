# alphahttpd

αhttpd is a small HTTP/1.1 file server.

Current features and limitations:

* Runs on Linux and FreeBSD (uses epoll and kqueue)
* HTTP/1.1 only
* Serves the file tree in `www/` directory by default
* Uses `index.html` as index file
* Doesn't use sendfile()
* No caching
* No ETag, Content-Type, If-None-Match, Range
* Single thread only
* stdout/stderr logging only
* SSE-optimized HTTP parser
* Basic command-line parameters; no configuration file
* Instant build and startup
* <100KB portable binary file
* <5000 lines of code (excluding dependencies)


Build on Linux:

	git clone https://github.com/stsaz/ffbase
	git clone https://github.com/stsaz/ffos
	git clone https://github.com/stsaz/alphahttpd
	cd alphahttpd
	make -j4

Build on FreeBSD:

	gmake -j4

Run:

	./alphahttpd

Use custom address and port:

	./alphahttpd -l 127.0.0.1:8080
