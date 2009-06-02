#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/param.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"
#include "../src/main.h"
#include "../src/cfg_file.h"
#include "../src/url.h"
#include "tests.h"

const char *test_text =
"www.schemeless.ru\n"
"www.schemeless.rus\n"
" as ftp.schemeless.ru dasd \n"
"ftp12.schemeless.ru\n"
"ftpsearch.schemeless.ru\n"
"schemeless.ru\n"
"www.schemeless.microsoft\n"
"1.2.3.4\n"
"1.2.3.4/a\n"
"1.2.3\n"
"1.2.3.4.5\n"
"www.schemeless.ru,\n"
"www.schemeless.ru.\n"
"http://www.schemed.ru.\n"
"http://www.schemed.ru.\n"
"http://www.bolinfest.com/targetalert/'\n"
"http://www.bolinfest.com/targetalert/'';\n"
"https://www.schemed.ru.\n"
"ufps://www.schemed.ru.\n"
"http://ported.ru:8080\n"
"http://ported.ru:8080\n"
"http://1.2.3.4\n"
"http://1.2.3.4:80\n"
"1.2.3.4:80\n"
"www.a9.com\n"
"www.a-9.com\n"
"http://www.schemed.ru/a.txt:\n"
"http://www.schemed.ru/a.txt'\n"
"http://www.schemed.ru/a.txt\"\n"
"http://www.schemed.ru/a.txt>\n"
"http://www.schemed.ru/a=3&b=4\n"
"http://spam.ru/bad=user@domain.com\n"
"http://spam.ru/bad=user@domain.com\n"
"http://spam.ru user@domain.com\n"
"http://a.foto.radikal.ru/0604/de7793c6ca62.jpg\n"
"http://a.foto.radikal.ru/0604/de7793c6ca62.jpg\n"
"schemeless.gz\n"
"schemeless.jp\n"
"schemeless.ua\n"
"schemeless.gz/a\n"
"mysql.so\n"
"http://mysql.so\n"
"3com.com\n"
"lj-user.livejournal.com\n"
"http://lj-user.livejournal.com\n"
"http://vsem.ru?action;\n";
const char *test_html = "<some_tag>This is test file with <a href=\"http://microsoft.com\">http://TesT.com/././?%45%46%20 url</a></some_tag>";

/* Function for using in glib test suite */
void
rspamd_url_test_func ()
{
	/* XXX: maybe write test for this */
}
