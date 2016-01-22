INSTALLATION: follow the installation guide of libcoap... autogen.sh, configure & make.

PUBSUB BROKER (SERVER) startup: go to /examples and launch pubsub-broker. Pubsub broker is based on the example server code provided by libcoap.

PUBSUB CLIENT: use libcoap's example client program in the /examples folder.

DISCOVER:
./coap-client -m get coap://[::1]/.well-known/core?rt=core.ps

CREATE:
./coap-client -m post -p 61616 -e "<sensor>;ct=0" -O 14,200 coap://[::1]/ps
options 14 = Max-Age (optional)
NOTE: the topic name should be inside '<>', followed by ';ct=content-type'

PUBLISH:
./coap-client -m put -p 61616 -e "sensor_value 123.45" -t 0 -O 14,200 coap://[::1]/ps/sensor
options 14 = Max-Age (optional)
-t content-type (mandatory, must match with the content-type given in CREATE)
-N = use non-confirmable (NON) message

READ:
 ./coap-client -m get -p 61616 -A 0 coap://[::1]/ps/sensor
-A content-type (optional; if used, must match with the content-type given in CREATE)

SUBSCRIBE:
./coap-client -m get -p 61616 -s 60 -A 0 coap://[::1]/ps/sensor
-s 60 (mandatory, subscription for n seconds)
-A content-type (optional; if used, must match with the content-type given in CREATE)


UNSUBSCRIBE:
./coap-client -m get -p 61616 -O 6,1 -A 0 coap://[::1]/ps/sensor
option 6 = Observe, value 1 = unsubscribe
-A content-type (if used, must match with the content-type given in CREATE)

REMOVE:
./coap-client -m delete -p 61616 coap://[::1]/ps/sensor


