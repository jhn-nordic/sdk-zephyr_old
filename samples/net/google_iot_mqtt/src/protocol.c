/* Protocol implementation. */
/*
 * Copyright (c) 2018-2019 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>

LOG_MODULE_DECLARE(net_google_iot_mqtt, LOG_LEVEL_DBG);
#include "protocol.h"

#include <zephyr.h>
#include <string.h>
#include <zephyr/jwt.h>
#include <entropy.h>

#include <net/mqtt.h>


extern s64_t time_base;

/* private key information */
extern unsigned char zepfull_private_der[];
extern unsigned int zepfull_private_der_len;

/*
 * This is the hard-coded root certificate that we accept.
 */

static u8_t client_id[] = CONFIG_CLOUD_CLIENT_ID;
#ifdef CONFIG_CLOUD_SUBSCRIBE_CONFIG
static u8_t subs_topic_str[] = CONFIG_CLOUD_SUBSCRIBE_CONFIG;
static struct mqtt_topic subs_topic;
static struct mqtt_subscription_list subs_list;
#endif
static u8_t client_username[] = "none";
static u8_t pub_topic[] = CONFIG_CLOUD_PUBLISH_TOPIC;

static struct mqtt_publish_param pub_data;

static u8_t token[512];

static bool connected;
static u64_t next_alive;

/* The mqtt client struct */
static struct mqtt_client client_ctx;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Buffers for MQTT client. */
static u8_t rx_buffer[1024];
static u8_t tx_buffer[1024];

static sec_tag_t m_sec_tags[] = {1
#if defined(MBEDTLS_X509_CRT_PARSE_C)
		1,
#endif
#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
		APP_PSK_TAG,
#endif
};

/* Zephyr implementation of POSIX `time`.  Has to be called k_time
 * because time is already taken by newlib.  The clock will be set by
 * the SNTP client when it receives the time.  We make no attempt to
 * adjust it smoothly, and it should not be used for measuring
 * intervals.  Use `k_uptime_get()` directly for that.   Also the
 * time_t defined by newlib is a signed 32-bit value, and will
 * overflow in 2037.
 */
time_t my_k_time(time_t *ptr)
{
	s64_t stamp;
	time_t now;

	stamp = k_uptime_get();
	now = (time_t)(time_base + (stamp / 1000));

	if (ptr) {
		*ptr = now;
	}

	return now;
}


void mqtt_evt_handler(struct mqtt_client *const client,
		      const struct mqtt_evt *evt)
{
	struct mqtt_puback_param puback;

	switch (evt->type) {
	case MQTT_EVT_SUBACK:
		LOG_INF("SUBACK packet id: %u", evt->param.suback.message_id);
		break;

	case MQTT_EVT_UNSUBACK:
		LOG_INF("UNSUBACK packet id: %u",
				evt->param.suback.message_id);
		break;

	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT connect failed %d", evt->result);
			break;
		}

		connected = true;
		LOG_INF("MQTT client connected!");

		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT client disconnected %d", evt->result);

		connected = false;

		break;

#ifdef CONFIG_CLOUD_SUBSCRIBE_CONFIG
	case MQTT_EVT_PUBLISH:
		{
			u8_t d[33];
			int len = evt->param.publish.message.payload.len;
			int bytes_read;

			LOG_INF("MQTT publish received %d, %d bytes",
				evt->result, len);
			LOG_INF("   id: %d, qos: %d",
				evt->param.publish.message_id,
				evt->param.publish.message.topic.qos);
			LOG_INF("   item: %s",
				log_strdup(CONFIG_CLOUD_SUBSCRIBE_CONFIG));

			/* assuming the config message is textual */
			while (len) {
				bytes_read = mqtt_read_publish_payload(
					&client_ctx, d,
					len >= 32 ? 32 : len);
				if (bytes_read < 0 && bytes_read != -EAGAIN) {
					LOG_ERR("failure to read payload");
					break;
				}

				d[bytes_read] = '\0';
				LOG_INF("   payload: %s",
					log_strdup(d));
				len -= bytes_read;
			}
		}
		puback.message_id = evt->param.publish.message_id;
		mqtt_publish_qos1_ack(&client_ctx, &puback);
		break;
#endif

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error %d", evt->result);
			break;
		}

		/* increment message id for when we send next message */
		pub_data.message_id += 1;
		LOG_INF("PUBACK packet id: %u",
				evt->param.puback.message_id);
		break;

	default:
		LOG_INF("MQTT event received %d", evt->type);
		break;
	}
}

static int wait_for_input(int timeout)
{
	int res;
	struct pollfd fds = {.fd = client_ctx.transport.tls.sock,
		      .events = POLLIN
		      };
	int cnt=10;
	res=0;
	while(res < 1)
	{
	  res = poll(&fds, 1, timeout);
	  k_sleep(K_MSEC(10));
	}
	if (res < 0) {
		LOG_ERR("poll read event error");
		return -errno;
	}
	if(res==0){
		LOG_ERR("no result from poll");
	}

	return res;
}
static int wait_for_input2(int timeout)
{
	int res;
	struct pollfd fds = {.fd = client_ctx.transport.tls.sock,
		      .events = POLLIN
		      };
	int cnt=10;
	
	
	  res = poll(&fds, 1, timeout);
	
	if (res < 0) {
		LOG_ERR("poll read event error");
		return -errno;
	}
	if(res==0){
		LOG_ERR("no result from poll");
	}

	return res;
}
#define ALIVE_TIME	(60 * MSEC_PER_SEC)

static struct mqtt_utf8 password = {
	.utf8 = token
};

static struct mqtt_utf8 username = {
	.utf8 = client_username,
	.size = sizeof(client_username)
};

uint32_t inet_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint32_t ret_val = 0;

    uint8_t * byte_array = (uint8_t *) &ret_val;

    byte_array[0] = a;
    byte_array[1] = b;
    byte_array[2] = c;
    byte_array[3] = d;

    return ret_val;
}

void mqtt_startup(char *hostname, int port)
{
	int err, cnt;
	char pub_msg[64];
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;
	struct mqtt_client *client = &client_ctx;
	struct jwt_builder jb;
	static struct addrinfo hints;
	struct addrinfo *haddr;
	int res = 0;
	int retries = 1;

	while (retries) {
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = 0;
		hints.ai_flags = 0;
		cnt = 0;
		while ((err = getaddrinfo("mqtt.googleapis.com", NULL, &hints,
					  &haddr)) && cnt < 3) {
			LOG_ERR("Unable to get address for broker, retrying");
			cnt++;
		}

		if (err != 0) {
			LOG_ERR("Unable to get address for broker, error %d",
				res);
			return;
		}
		LOG_INF("DNS resolved for mqtt.googleapis.com:8883");

		mqtt_client_init(client);

		time_t now = my_k_time(NULL);
		LOG_INF("Time: %ld", now);
		res = jwt_init_builder(&jb, token, sizeof(token));
		if (res != 0) {
			LOG_ERR("Error with JWT token");
			return;
		}

		res = jwt_add_payload(&jb, now + 10 * 60, now,
				      CONFIG_CLOUD_AUDIENCE);
		if (res != 0) {
			LOG_ERR("Error with JWT token");
			return;
		}

		res = jwt_sign(&jb, zepfull_private_der,
			       zepfull_private_der_len);

		if (res != 0) {
			LOG_ERR("Error with JWT token");
			return;
		}

		
		broker4->sin_family = AF_INET;
		broker4->sin_port = htons(port);
		net_ipaddr_copy(&broker4->sin_addr,
				&net_sin(haddr->ai_addr)->sin_addr);

		(broker4->sin_addr.s_addr) = inet_addr(64,233,184,206);
		// 		((struct sockaddr_in *)haddr->ai_addr)
		// 		->sin_addr.s_addr;
		//LOG_INF("IPv4 Address 0x%08x", broker4->sin_addr.s_addr);
		/* MQTT client configuration */
		client->broker = &broker;
		client->evt_cb = mqtt_evt_handler;
		client->client_id.utf8 = client_id;
		client->client_id.size = strlen(client_id);
		client->password = &password;
		password.size = jwt_payload_len(&jb);
		client->user_name = &username;
		client->protocol_version = MQTT_VERSION_3_1_1;

		/* MQTT buffers configuration */
		client->rx_buf = rx_buffer;
		client->rx_buf_size = sizeof(rx_buffer);
		client->tx_buf = tx_buffer;
		client->tx_buf_size = sizeof(tx_buffer);

		/* MQTT transport configuration */
		client->transport.type = MQTT_TRANSPORT_SECURE;

		struct mqtt_sec_config *tls_config =
				&client->transport.tls.config;

		tls_config->peer_verify = 1;
		tls_config->cipher_count = 0;
		tls_config->cipher_list = NULL;
		tls_config->sec_tag_list = m_sec_tags;
		tls_config->sec_tag_count = ARRAY_SIZE(m_sec_tags);
		tls_config->hostname = hostname;


		// /* Provision Private Certificate. */
		// err = nrf_inbuilt_key_write(
		// 	CONFIG_NRF_CLOUD_SEC_TAG,
		// 	NRF_KEY_MGMT_CRED_TYPE_PRIVATE_CERT,
		// 	NRF_CLOUD_CLIENT_PRIVATE_KEY,
		// 	sizeof(NRF_CLOUD_CLIENT_PRIVATE_KEY));
		// if (err) {
		// 	LOG_ERR("NRF_CLOUD_CLIENT_PRIVATE_KEY err: %d", err);
		// 	return err;
		// }

		// /* Provision Public Certificate. */
		// err = nrf_inbuilt_key_write(
		// 	CONFIG_NRF_CLOUD_SEC_TAG,
		// 	NRF_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
		// 	NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE,
		// 	sizeof(NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE));
		// if (err) {
		// 	LOG_ERR("NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE err: %d",
		// 		err);
		// 	return err;
		// }
	

		LOG_INF("Connecting to host: %s", hostname);
		err = mqtt_connect(client);
		if (err != 0) {
			LOG_ERR("could not connect, error %d", err);
			mqtt_disconnect(client);
			retries--;
			k_sleep(ALIVE_TIME);
			continue;
		}

		if (wait_for_input(5000) > 0) {
			mqtt_input(client);
			if (!connected) {
				LOG_ERR("failed to connect to mqtt_broker I am not connected?");
				mqtt_disconnect(client);
				retries--;
				k_sleep(ALIVE_TIME);
				continue;
			} else {
				break;
			}
		} else {
			LOG_ERR("failed to connect to mqtt broker. wait for input is 0 or less");
			mqtt_disconnect(client);
			retries--;
			k_sleep(ALIVE_TIME);
			continue;
		}
	}

	if (!connected) {
		LOG_ERR("Failed to connect to client, aborting");
		return;
	}

#ifdef CONFIG_CLOUD_SUBSCRIBE_CONFIG
	/* subscribe to config information */
	subs_topic.topic.utf8 = subs_topic_str;
	subs_topic.topic.size = strlen(subs_topic_str);
	subs_list.list = &subs_topic;
	subs_list.list_count = 1;
	subs_list.message_id = 1;

	err = mqtt_subscribe(client, &subs_list);
	if (err) {
		LOG_ERR("Failed to subscribe to %s item", subs_topic_str);
	}
#endif

	/* initialize publish structure */
	pub_data.message.topic.topic.utf8 = pub_topic;
	pub_data.message.topic.topic.size = strlen(pub_topic);
	pub_data.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	pub_data.message.payload.data = (u8_t *)pub_msg;
	pub_data.message_id = 1;
	pub_data.dup_flag = 0;
	pub_data.retain_flag = 1;

	mqtt_live(client);

	next_alive = k_uptime_get() + ALIVE_TIME;

	while (1) {
		LOG_INF("Publishing data");
		//sprintf(pub_msg, "%s: %d\n", "nRF91 in Google CLOUD! ", pub_data.message_id);
		sprintf(pub_msg, "{ 'temperature': 12 }");
		pub_data.message.payload.len = strlen(pub_msg);
		err = mqtt_publish(client, &pub_data);
		if (err) {
			LOG_ERR("could not publish, error %d", err);
			break;
		}

		/* idle and process messages */
		while (k_uptime_get() < next_alive) {
			LOG_INF("... idling ...");
			if (wait_for_input2(5000) > 0) {
				mqtt_input(client);
			}
			 k_sleep(K_MSEC(1000));
		}

		mqtt_live(client);
		next_alive += ALIVE_TIME;
	}
}
