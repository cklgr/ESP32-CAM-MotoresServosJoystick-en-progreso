#define MY_SSID  "WIFI";
#define MY_PASSWORD  "CLAVE";
//#define MY_DUCKDNS_TOKEN   "xxxxxx-xxxx-xxxx-xxxx-xxxxxx"

// Set your Static IP address
IPAddress local_IP(192, 168, 1, 252);
// Set your Gateway IP address
IPAddress gateway(192, 168, 1, 1);

IPAddress subnet(255, 255, 0, 0);
IPAddress primaryDNS(8, 8, 8, 8); // optional
IPAddress secondaryDNS(8, 8, 4, 4); // optional
