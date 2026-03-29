/*
 * rwd_signaling.c -- WHIP HTTP signaling endpoint
 *
 * Minimal HTTP handler for WebRTC-HTTP Ingestion Protocol:
 *   GET  /webrtc  → serve player HTML page
 *   POST /whip    → SDP offer/answer exchange
 *   DELETE /whip/{session} → teardown session
 *
 * Also handles CORS preflight (OPTIONS) for cross-origin requests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rwd.h"

/* ── Embedded WebRTC player page ── */

static const char webrtc_html[] =
	"<!DOCTYPE html>\n"
	"<html><head><meta charset='utf-8'>\n"
	"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
	"<title>Raptor WebRTC</title>\n"
	"<style>body{margin:0;background:#111;display:flex;flex-direction:column;"
	"align-items:center;justify-content:center;min-height:100vh;font-family:sans-serif}"
	"video{width:100%;max-height:90vh;object-fit:contain;background:#000}"
	"button{margin:10px;padding:8px 24px;font-size:16px;cursor:pointer;"
	"border:1px solid #555;background:#222;color:#fff;border-radius:4px}"
	"button:hover{background:#333}#status{color:#888;font-size:14px}</style>\n"
	"</head><body>\n"
	"<video id='v' autoplay muted playsinline></video>\n"
	"<div><button id='btn' onclick='toggle()'>Connect</button>"
	"<button id='mute' onclick='toggleMute()' style='display:none'>Unmute</button>"
	"<span id='status'></span></div>\n"
	"<script>\n"
	"let pc=null,resource=null;\n"
	"const v=document.getElementById('v'),btn=document.getElementById('btn'),"
	"st=document.getElementById('status');\n"
	"async function start(){\n"
	"  st.textContent='Connecting...';\n"
	"  pc=new RTCPeerConnection({iceServers:[]});\n"
	"  pc.addTransceiver('video',{direction:'recvonly'});\n"
	"  pc.addTransceiver('audio',{direction:'recvonly'});\n"
	"  pc.ontrack=e=>{if(!v.srcObject){v.srcObject=new MediaStream()}v.srcObject.addTrack(e.track);v.muted=true;v.play().catch(()=>{})};\n"
	"  pc.oniceconnectionstatechange=()=>{st.textContent=pc.iceConnectionState;"
	"if(pc.iceConnectionState==='failed'||pc.iceConnectionState==='disconnected')stop()};\n"
	"  const offer=await pc.createOffer();\n"
	"  await pc.setLocalDescription(offer);\n"
	"  /* Wait for ICE gathering to complete */\n"
	"  await new Promise(r=>{if(pc.iceGatheringState==='complete')r();"
	"else pc.onicegatheringstatechange=()=>{if(pc.iceGatheringState==='complete')r()}});\n"
	"  const resp=await fetch('/whip',{method:'POST',"
	"headers:{'Content-Type':'application/sdp'},body:pc.localDescription.sdp});\n"
	"  if(!resp.ok){st.textContent='Error: '+resp.status;return}\n"
	"  resource=resp.headers.get('Location');\n"
	"  const sdp=await resp.text();\n"
	"  await pc.setRemoteDescription({type:'answer',sdp});\n"
	"  btn.textContent='Disconnect';document.getElementById('mute').style.display='';\n"
	"}\n"
	"async function stop(){\n"
	"  if(resource)fetch(resource,{method:'DELETE'}).catch(()=>{});\n"
	"  if(pc)pc.close();\n"
	"  pc=null;resource=null;v.srcObject=null;\n"
	"  btn.textContent='Connect';st.textContent='';document.getElementById('mute').style.display='none';\n"
	"}\n"
	"function toggle(){pc?stop():start()}\n"
	"function toggleMute(){v.muted=!v.muted;document.getElementById('mute').textContent=v.muted?'Unmute':'Mute'}\n"
	"</script></body></html>\n";

/* ── HTTP response helpers ── */

static void http_send(int fd, const char *status, const char *content_type, const char *body,
		      size_t body_len, const char *extra_headers)
{
	char hdr[1024];
	int hdr_len = snprintf(hdr, sizeof(hdr),
			       "HTTP/1.1 %s\r\n"
			       "Content-Type: %s\r\n"
			       "Content-Length: %zu\r\n"
			       "Access-Control-Allow-Origin: *\r\n"
			       "Access-Control-Allow-Methods: POST, DELETE, OPTIONS\r\n"
			       "Access-Control-Allow-Headers: Content-Type\r\n"
			       "%s"
			       "Connection: close\r\n"
			       "\r\n",
			       status, content_type, body_len, extra_headers ? extra_headers : "");

	(void)!write(fd, hdr, hdr_len);
	if (body && body_len > 0)
		(void)!write(fd, body, body_len);
}

static void http_error(int fd, const char *status)
{
	http_send(fd, status, "text/plain", status, strlen(status), NULL);
}

/* ── Generate session ID ── */

static void generate_session_id(char *out, size_t out_size)
{
	uint8_t bytes[RWD_SESSION_ID_LEN];
	rwd_random_bytes(bytes, sizeof(bytes));
	for (int i = 0; i < RWD_SESSION_ID_LEN && (size_t)(i * 2 + 2) < out_size; i++)
		snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

/* ── WHIP POST handler: SDP offer → answer ── */

static void handle_whip_post(rwd_server_t *srv, int fd, const char *body,
			     size_t body_len __attribute__((unused)),
			     const struct sockaddr_storage *local_addr)
{
	/* Parse SDP offer */
	rwd_sdp_offer_t offer;
	if (rwd_sdp_parse_offer(body, &offer) != 0) {
		http_error(fd, "400 Bad Request");
		return;
	}

	/* Check client limit */
	pthread_mutex_lock(&srv->clients_lock);
	if (srv->client_count >= srv->max_clients) {
		pthread_mutex_unlock(&srv->clients_lock);
		http_error(fd, "503 Service Unavailable");
		return;
	}

	/* Allocate client */
	rwd_client_t *c = calloc(1, sizeof(*c));
	if (!c) {
		pthread_mutex_unlock(&srv->clients_lock);
		http_error(fd, "500 Internal Server Error");
		return;
	}

	c->server = srv;
	c->active = true;
	c->created_at = rss_timestamp_us();
	memcpy(&c->offer, &offer, sizeof(offer));
	rss_strlcpy(c->remote_ufrag, offer.ice_ufrag, sizeof(c->remote_ufrag));
	rss_strlcpy(c->remote_pwd, offer.ice_pwd, sizeof(c->remote_pwd));

	/* Generate local ICE credentials */
	rwd_generate_ice_credentials(c->local_ufrag, sizeof(c->local_ufrag), c->local_pwd,
				     sizeof(c->local_pwd));

	/* Generate session ID */
	generate_session_id(c->session_id, sizeof(c->session_id));

	/* Initialize DTLS context for this client */
	if (rwd_dtls_client_init(c, srv->dtls) != 0) {
		free(c);
		pthread_mutex_unlock(&srv->clients_lock);
		http_error(fd, "500 Internal Server Error");
		return;
	}

	/* Detect local IP from the HTTP socket if not already set */
	if (!srv->local_ip[0] && local_addr) {
		if (local_addr->ss_family == AF_INET) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)local_addr)->sin_addr, srv->local_ip,
				  sizeof(srv->local_ip));
		} else if (local_addr->ss_family == AF_INET6) {
			struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)local_addr;
			/* Check for IPv4-mapped IPv6 */
			if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr))
				inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], srv->local_ip,
					  sizeof(srv->local_ip));
			else
				inet_ntop(AF_INET6, &a6->sin6_addr, srv->local_ip,
					  sizeof(srv->local_ip));
		}
	}

	/* Generate SDP answer */
	char sdp_answer[RWD_SDP_BUF_SIZE];
	int sdp_len = rwd_sdp_generate_answer(c, srv, sdp_answer, sizeof(sdp_answer));
	if (sdp_len < 0) {
		rwd_dtls_client_free(c);
		free(c);
		pthread_mutex_unlock(&srv->clients_lock);
		http_error(fd, "500 Internal Server Error");
		return;
	}

	/* Add to client list */
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		if (!srv->clients[i]) {
			srv->clients[i] = c;
			srv->client_count++;
			break;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);

	/* Send 201 Created with SDP answer */
	char location[128];
	snprintf(location, sizeof(location), "Location: /whip/%s\r\n", c->session_id);
	http_send(fd, "201 Created", "application/sdp", sdp_answer, sdp_len, location);

	RSS_INFO("WHIP: session %s created (video_pt=%d audio_pt=%d)", c->session_id,
		 c->offer.video_pt, c->offer.audio_pt);
}

/* ── WHIP DELETE handler: teardown session ── */

static void handle_whip_delete(rwd_server_t *srv, int fd, const char *session_id)
{
	pthread_mutex_lock(&srv->clients_lock);
	for (int i = 0; i < RWD_MAX_CLIENTS; i++) {
		if (srv->clients[i] && strcmp(srv->clients[i]->session_id, session_id) == 0) {
			rwd_client_t *c = srv->clients[i];
			rwd_media_teardown(c);
			rwd_dtls_client_free(c);
			free(c);
			srv->clients[i] = NULL;
			srv->client_count--;
			RSS_INFO("WHIP: session %s deleted", session_id);
			break;
		}
	}
	pthread_mutex_unlock(&srv->clients_lock);

	http_send(fd, "200 OK", "text/plain", "OK", 2, NULL);
}

/* ── Main HTTP request handler ── */

void rwd_signaling_handle(rwd_server_t *srv, int client_fd,
			  const struct sockaddr_storage *local_addr)
{
	/* Read the full HTTP request (simple: single read, small requests) */
	char buf[RWD_HTTP_BUF_SIZE];
	ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		close(client_fd);
		return;
	}
	buf[n] = '\0';

	/* Parse request line */
	char method[16], path[256];
	if (sscanf(buf, "%15s %255s", method, path) != 2) {
		http_error(client_fd, "400 Bad Request");
		close(client_fd);
		return;
	}

	/* Reject oversized requests */
	if (n >= (ssize_t)(sizeof(buf) - 1)) {
		http_error(client_fd, "414 URI Too Long");
		close(client_fd);
		return;
	}

	/* CORS preflight */
	if (strcmp(method, "OPTIONS") == 0) {
		http_send(client_fd, "204 No Content", "text/plain", NULL, 0, NULL);
		close(client_fd);
		return;
	}

	/* GET /webrtc — serve player page */
	if (strcmp(method, "GET") == 0 && strcmp(path, "/webrtc") == 0) {
		http_send(client_fd, "200 OK", "text/html; charset=utf-8", webrtc_html,
			  sizeof(webrtc_html) - 1, "Cache-Control: no-cache\r\n");
		close(client_fd);
		return;
	}

	/* POST /whip — SDP offer/answer */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/whip") == 0) {
		/* Parse Content-Length */
		size_t content_length = 0;
		const char *cl = strcasestr(buf, "Content-Length:");
		if (cl)
			content_length = (size_t)atoi(cl + 15);

		/* Find body (after \r\n\r\n) */
		const char *body = strstr(buf, "\r\n\r\n");
		if (!body) {
			RSS_WARN("WHIP: no body separator in request");
			http_error(client_fd, "400 Bad Request");
			close(client_fd);
			return;
		}
		body += 4;
		size_t body_len = n - (body - buf);

		/* Read remaining body if we didn't get it all */
		size_t need = content_length > 0 ? content_length : 0;
		while (body_len < need && (size_t)n < sizeof(buf) - 1) {
			ssize_t more = read(client_fd, buf + n, sizeof(buf) - 1 - n);
			if (more <= 0)
				break;
			n += more;
			buf[n] = '\0';
			body_len = n - (body - buf);
		}

		handle_whip_post(srv, client_fd, body, body_len, local_addr);
		close(client_fd);
		return;
	}

	/* DELETE /whip/{session} — teardown */
	if (strcmp(method, "DELETE") == 0 && strncmp(path, "/whip/", 6) == 0) {
		const char *session_id = path + 6;
		if (strlen(session_id) > 0) {
			handle_whip_delete(srv, client_fd, session_id);
			close(client_fd);
			return;
		}
	}

	http_error(client_fd, "404 Not Found");
	close(client_fd);
}
