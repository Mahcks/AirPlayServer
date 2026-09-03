#ifndef GLOBAL_H
#define GLOBAL_H

/*
 * MirrorSim is a screen-mirroring receiver, not a remote URL/HLS video player.
 * Keep AirPlay video (bit 0), HLS (bit 4), and screen rotation (bit 8) clear so
 * apps do not hand standalone playback to callbacks this runtime cannot serve.
 */
#define GLOBAL_FEATURES 0x5A7FFEE6
#define GLOBAL_FEATURES_TXT "0x5A7FFEE6,0x0"
#define GLOBAL_FEATURES_DECIMAL "1518337766"
#define GLOBAL_MODEL    "AppleTV14,1"
#define GLOBAL_VERSION  "845.5.1"

#define MAX_HWADDR_LEN 6

#endif
