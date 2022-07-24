#include <stdarg.h>
/*Very useful function, inspired by perl :)*/
static void die(char *reason, ...)
{
	va_list args;

	va_start(args, reason);
	assert(vfprintf(stderr, reason, args) >= 0);
	va_end(args);

	exit(1);
}

/*protected allocation: Basically, it crashes the program if memory can't be allocated*/
void *palloc(size_t members, size_t element)
{
	void *ret;

	ret = calloc(members, element);
	if (!ret)
		die("Failed to allocate memory.\n");

	return ret;
}

/*When invertfunc is true, it runs the length of data, and returns the position including the first
  linear white space character, which is set to null in the function that calls this.*/
/*When invertfunc is false, it skips all white space and returns an offset to non-white space data*/
size_t skip_lws(char *header, size_t offset, bool invertfunc)
{
	char c;
	size_t i;

	for (i = offset; (i < headerlen) && (c = header[i]); i++) {
		switch (c) {
		/*These are the 4 linear white space characters valid in http*/
		case ' ' :
		case '\r':
		case '\n':
		case '\t':
			if (invertfunc)
				return i;
			else
				continue;
/*		Covered in for loop condition.*/
/*		case '\0':
			return i;*/
		default:
			if (invertfunc)
				continue;
			else
				return i;
		}
	}

	return i;
}

/*Tokenizes request header, without allocating data, by using the request header*/
bool header_tokenize(client_data_t *cdata)
{
	char* request;
	token tmptoken;
	size_t pos, tokenpos;

	request = cdata->request;
	for (pos = 0, tokenpos = 0; (pos < cdata->request_recvd) &&
	    (pos < headerlen) && (tokenpos < maxtokens); tokenpos++, pos++) {
		/*Skips the linear white space and returns the starting position of the data*/
		pos = skip_lws(request, pos, false);

		tmptoken.str = request + pos;
		/*Length of the string, including one white space character*/
		tmptoken.len = skip_lws(request, pos, true) - pos;

		/*updates the current position in the header*/
		pos += tmptoken.len;

		cdata->tokens[tokenpos] = tmptoken;

		/*Writes the null byte to the last character in the token string, which is linear
		 white space.*/
		if (pos < headerlen)
			request[pos] = '\0';
		else
			/*Not a valid request, so false is returned;
			  however, this function doesn't exactly check for
			  correctness of http requests.*/
			return false;

		/*This if statement determines if the end of the request has been reached*/
		/*This could be in the for loop, but I don't want to make that any longer*/
		if (((pos+1) >= headerlen) || (request[pos+1] == '\0'))
			break;
	}

	/*When the loop ends, tokenpos will go from a location to the amount of tokens
	  because of tokenpos++*/
	cdata->tokenslen = tokenpos;
	return true;
}

/*appends strings to the end of the current position in the request header*/
void strappend(client_data_t *cdata, const char *str)
{
	size_t len, resp_diff;

	/*I know strlen is a bad idea*/
	len       = strlen(str);
	resp_diff = (headerlen - cdata->responselen);

	/*Copies string over to the response header*/
	strncpy((cdata->response + cdata->responselen), str,
		(resp_diff < len) ? resp_diff : len);
	cdata->responselen += len;
}

void uintappend(client_data_t *cdata, size_t unum)
{
	size_t i, len;
	size_t reversed_unum;

	/*Flips integer for writing to string*/
	for (i = 0, reversed_unum = 0; unum > 0; i++) {
		reversed_unum *= 10;
		reversed_unum += unum % 10;
		unum /= 10;
	}

	len = i;

	/*Writes the flipped integer*/
	for (i = 0; (i < len) && ((cdata->responselen+i) < (headerlen-1)); i++) {
		cdata->response[cdata->responselen+i] = (char)((reversed_unum % 10) + '0');
		reversed_unum /= 10;
	}

	cdata->responselen += i;
}

/*_XOPEN_SOURCE is required for strptime*/
#define _XOPEN_SOURCE
#include <time.h>

/*0 Jan, 1 Feb, 2 Mar, 3 Apr, 4 May, 5 Jun,
  6 Jul, 7 Aug, 8 Sep, 9 Oct, 10 Nov, 11 Dec*/
char *month[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
/*0 Sun, 1 Mon, 2 Tue, 3 Wed, 4 Thu, 5 Fri, 6 Sat*/
char *wkday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
/*Same as above, just the expanded version*/
char *weekday[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

/*rfc1123-date example from spec = Sun, 06 Nov 1994 08:49:37 GMT*/
/*Writes date in rfc1123-date format, the preferred format in the http/1.1 specification*/
void dateappend(client_data_t *cdata, time_t sec)
{
	/*Struct holding decoded epoch in Georgian calendar time*/
	struct tm caltime;

	if (!gmtime_r(&sec, &caltime))
		die("gmtime_r should never have failed\n");

	/*Sun, */
	strappend(cdata, wkday[caltime.tm_wday]);
	strappend(cdata, ", ");

	/*0*/
	if (caltime.tm_mday < 10)
		uintappend(cdata, 0);
	/*6*/
	uintappend(cdata, caltime.tm_mday);

	/* Nov */
	strappend(cdata, " ");
	strappend(cdata, month[caltime.tm_mon]);
	strappend(cdata, " ");

	/*1994*/
	/*Only supports years 1k onward, and tm_year is the years since 1900*/
	uintappend(cdata, caltime.tm_year + 1900);

	/* */
	strappend(cdata, " ");

	/*0*/
	if (caltime.tm_hour < 10)
		uintappend(cdata, 0);
	/*8*/
	uintappend(cdata, caltime.tm_hour);

	/*:*/
	strappend(cdata, ":");

	/*0*/
	if (caltime.tm_min < 10)
		uintappend(cdata, 0);
	/*49*/
	uintappend(cdata, caltime.tm_min);

	/*:*/
	strappend(cdata, ":");

	/*0*/
	if (caltime.tm_sec < 10)
		uintappend(cdata, 0);
	/*37*/
	uintappend(cdata, caltime.tm_sec);

	/* GMT*/
	strappend(cdata, " GMT");
}

/*Parses tokenized request and generates a response*/
bool gen_response(client_data_t *cdata)
{
	size_t i;
	token tok;
	bool get, head;
	struct stat fileinfo;

	/*Initialize response variables*/
	cdata->responselen   = 0;
	cdata->response_sent = 0;

	/*A minimum of three is required for any verb*/
	if (cdata->tokenslen < 3) {
		cdata->keepalive = false;
		return false;
	}

	/*First token is the verb*/
	tok = cdata->tokens[0];

	get  = !strncmp(tok.str,  "GET", tok.len);
	head = !strncmp(tok.str, "HEAD", tok.len);

	tok = cdata->tokens[2];
	if (!strcmp(tok.str, "http/1.0"))
		cdata->keepalive = false;
	else
		/*Must be assumed true unless specified otherwise
		  according to the http 1.1 spec, and I assume that includes 1.2*/
		cdata->keepalive = true;

	/*Write version*/
	strappend(cdata, "HTTP/1.1 ");

	/*Minimum first three tokens are part of the status line*/
	for (i = 3; i < cdata->tokenslen; i++) {
		tok = cdata->tokens[i];

		/*Is http spec is case insensitive, and if so are all of implementations?*/
		if (!strncmp(tok.str, "Connection:", tok.len)) {
			i++;
			tok = cdata->tokens[i];

			if (!strcmp(tok.str, "close"))
				cdata->keepalive = false;
			else if (!strcmp(tok.str, "Keep-Alive"))
				cdata->keepalive = true;
		}
	}

	/*Check of supported http verbs*/
	if (!get && !head) {
		strappend(cdata, "501 Not Implemented\r\n");

		goto conn_status;
	}

	/*File path, or at least it should be*/
	tok = cdata->tokens[1];

	/*Probe for file information*/
	if (stat(tok.str, &fileinfo) < 0) {
		/*More informative output can be added later
		  on using different http error codes depending on errno values*/
		strappend(cdata, "404 File Not Found\r\n");

		goto conn_status;
	}

	/*I believe this is required by http*/
	if (!fileinfo.st_size) {
		cdata->readfile = false;
		/*I believe this is what is supposed to returned for zero length files*/
		strappend(cdata, "204 No Content\r\n");

		goto last_mod;
	}

	/*HEAD verb requires the same behaviour as GET
	  without actually sending anything but the header*/
	if (head) {
		cdata->readfile = false;
		goto OK;
	}

	/*tok.str in this case is the filename*/
	if ((cdata->rfd = open(tok.str, O_RDONLY | O_NONBLOCK)) < 0) {
		/*Could use errno to get better error
		  messages, but that's a project for later*/
		strappend(cdata, "403 Forbidden\r\n");
		cdata->readfile = false;
		goto conn_status;
	} else {
		cdata->readfile = true;
		goto OK;
	}

	/*200 OK*/
	OK:
		/*It's weird that this is the only response in the
		spec where the word(s) following the status code is all caps*/
		strappend(cdata, "200 OK\r\n");

		/*Different things can be set if range support gets added*/
		cdata->offset   = 0;
		cdata->tosend   = fileinfo.st_size;

	/*Last modified header*/
	last_mod:
		/*Write Last-Modified header for caching purposes*/
		strappend (cdata, "Last-Modified: ");
		dateappend(cdata, fileinfo.st_mtime);
		strappend (cdata, "\r\n");


	/*Last bit of meta data*/
/*	meta:*/
		/*These variables could be set to
		other values if range support gets added*/
		cdata->offset = 0;
		cdata->tosend = fileinfo.st_size;

		/*Not required, but is a general service to
		the client to include this, for caching and
		verifying that the client got the file correctly*/
		strappend (cdata, "Content-Length: ");
		uintappend(cdata, cdata->tosend);
		strappend(cdata, "\r\n");

	/*Pretty much does the closing argument*/
	conn_status:
		/*Might be useful for debugging*/
		strappend(cdata, "Server: httpc\r\n");

		/*Post connection status*/
		if (cdata->keepalive)
			strappend(cdata, "Connection: Keep-Alive\r\n\r\n");
		else
			strappend(cdata, "Connection: close\r\n\r\n");

	return true;
}

