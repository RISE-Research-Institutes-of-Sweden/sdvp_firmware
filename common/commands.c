/*
	Copyright 2016 - 2018 Benjamin Vedder	benjamin@vedder.se
	          2020        Marvin Damschen	marvin.damschen@ri.se

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "commands.h"
#include "commands_specific.h"
#include "packet.h"
#include "buffer.h"
#include "conf_general.h"
#include "datatypes.h"
#include "utils.h"
#include "log.h"
#include "terminal.h"
#include "comm_serial.h"
#include "pos.h"
#include "pos_gnss.h"
#include "timeout.h"
#include "autopilot.h"
#include "motor_sim.h"
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#define RTCM3PREAMB              0xD3

// Private variables
static uint8_t m_send_buffer[PACKET_MAX_PL_LEN];
static void(*m_send_func)(unsigned char *data, unsigned int len) = 0;

/**
 * Provide a function to use the next time there are packets to be sent.
 *
 * @param func
 * A pointer to the packet sending function.
 */
void commands_set_send_func(void(*func)(unsigned char *data, unsigned int len)) {
	m_send_func = func;
}

/**
 * Send a packet using the set send function.
 *
 * @param data
 * The packet data.
 *
 * @param len
 * The data length.
 */
void commands_send_packet(unsigned char *data, unsigned int len) {
	if (m_send_func) {
		m_send_func(data, len);
	}
}

/**
 * Process a received buffer with commands and data.
 *
 * @param data
 * The buffer to process.
 *
 * @param len
 * The length of the buffer.
 *
 * @param func
 * A pointer to the packet sending function.
 */
void commands_process_packet(unsigned char *data, unsigned int len,
		void (*func)(unsigned char *data, unsigned int len)) {

	if (!len) {
		return;
	}
	// Note: RTCM3PREAMB might be send when rtcm3_simple is involved. Should not be the case anymore (with F9P)
	if (data[0] == RTCM3PREAMB) {
		commands_printf("Warning: got unhandled RTCM3PREAMB");
		return;
	}

	uint8_t receiver_id = data[0];
	CMD_PACKET packet_id = data[1];
	data+=2;
	len-=2;

	if (receiver_id == main_id || receiver_id == ID_ALL || receiver_id == ID_CAR_CLIENT) {
		int id_ret = main_id;

		if (receiver_id == ID_CAR_CLIENT) {
			id_ret = ID_CAR_CLIENT;
		}

		// General commands
		switch (packet_id) {
		case CMD_HEARTBEAT: {
			timeout_reset();
		} break;

		case CMD_TERMINAL_CMD: {
			commands_set_send_func(func);

			data[len] = '\0';
			terminal_process_string((char*)data);
		} break;

		case CMD_SET_POS:
		case CMD_SET_POS_ACK: {
			float x, y, angle;
			int32_t ind = 0;
			x = buffer_get_float32(data, 1e4, &ind);
			y = buffer_get_float32(data, 1e4, &ind);
			angle = buffer_get_float32(data, 1e6, &ind);
			pos_set_xya(x, y, angle);

			if (packet_id == CMD_SET_POS_ACK) {
				commands_set_send_func(func);
				// Send ack
				int32_t send_index = 0;
				m_send_buffer[send_index++] = id_ret;
				m_send_buffer[send_index++] = packet_id;
				commands_send_packet(m_send_buffer, send_index);
			}
		} break;

		case CMD_SET_ENU_REF: {
			commands_set_send_func(func);

			int32_t ind = 0;
			double lat, lon, height;
			lat = buffer_get_double64(data, D(1e16), &ind);
			lon = buffer_get_double64(data, D(1e16), &ind);
			height = buffer_get_float32(data, 1e3, &ind);
			pos_gnss_set_enu_ref(lat, lon, height);

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_GET_ENU_REF: {
			timeout_reset();
			commands_set_send_func(func);

			double llh[3];
			pos_gnss_get_enu_ref(llh);

			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = CMD_GET_ENU_REF;
			buffer_append_double64(m_send_buffer, llh[0], D(1e16), &send_index);
			buffer_append_double64(m_send_buffer, llh[1], D(1e16), &send_index);
			buffer_append_float32(m_send_buffer, llh[2], 1e3, &send_index);
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_ADD_POINTS: {
			commands_set_send_func(func);

			int32_t ind = 0;
			bool first = true;

			while (ind < (int32_t)len) {
				ROUTE_POINT p;
				p.px = buffer_get_float32(data, 1e4, &ind);
				p.py = buffer_get_float32(data, 1e4, &ind);
				p.pz = buffer_get_float32(data, 1e4, &ind);
				p.speed = buffer_get_float32(data, 1e6, &ind);
				p.time = buffer_get_int32(data, &ind);
				p.attributes = buffer_get_uint32(data, &ind);
				bool res = autopilot_add_point(&p, first);
				first = false;

				if (!res) {
					break;
				}
			}

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_REMOVE_LAST_POINT: {
			commands_set_send_func(func);

			autopilot_remove_last_point();

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_CLEAR_POINTS: {
			commands_set_send_func(func);

			autopilot_clear_route();

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_GET_ROUTE_PART: {
			int32_t ind = 0;
			int first = buffer_get_int32(data, &ind);
			int num = data[ind++];

			if (num > 20) {
				break;
			}

			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = CMD_AP_GET_ROUTE_PART;

			int route_len = autopilot_get_route_len();
			buffer_append_int32(m_send_buffer, route_len, &send_index);

			for (int i = first;i < (first + num);i++) {
				ROUTE_POINT rp = autopilot_get_route_point(i);
				buffer_append_float32_auto(m_send_buffer, rp.px, &send_index);
				buffer_append_float32_auto(m_send_buffer, rp.py, &send_index);
				buffer_append_float32_auto(m_send_buffer, rp.pz, &send_index);
				buffer_append_float32_auto(m_send_buffer, rp.speed, &send_index);
				buffer_append_int32(m_send_buffer, rp.time, &send_index);
				buffer_append_uint32(m_send_buffer, rp.attributes, &send_index);
			}

			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_SET_ACTIVE: {
			commands_set_send_func(func);

			autopilot_set_active(data[0]);
			if (data[1])
				autopilot_reset_state();

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_REPLACE_ROUTE: {
			commands_set_send_func(func);

			int32_t ind = 0;
			int first = true;

			while (ind < (int32_t)len) {
				ROUTE_POINT p;
				p.px = buffer_get_float32(data, 1e4, &ind);
				p.py = buffer_get_float32(data, 1e4, &ind);
				p.pz = buffer_get_float32(data, 1e4, &ind);
				p.speed = buffer_get_float32(data, 1e6, &ind);
				p.time = buffer_get_int32(data, &ind);
				p.attributes = buffer_get_uint32(data, &ind);

				if (first) {
					first = !autopilot_replace_route(&p);
				} else {
					autopilot_add_point(&p, false);
				}
			}

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_AP_SYNC_POINT: {
			commands_set_send_func(func);

			int32_t ind = 0;
			int32_t point = buffer_get_int32(data, &ind);
			int32_t time = buffer_get_int32(data, &ind);
			int32_t min_diff = buffer_get_int32(data, &ind);

			autopilot_sync_point(point, time, min_diff);

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_SEND_RTCM_USB: {
			// NOTE: transfer to u-blox handled in comm_serial to minimize delay
			pos_gnss_input_rtcm3(data, len);
		} break;

		case CMD_SET_YAW_OFFSET:
		case CMD_SET_YAW_OFFSET_ACK: {
			float angle;
			int32_t ind = 0;
			angle = buffer_get_float32(data, 1e6, &ind);
			pos_set_yaw_offset(angle);

			if (packet_id == CMD_SET_YAW_OFFSET_ACK) {
				commands_set_send_func(func);
				// Send ack
				int32_t send_index = 0;
				m_send_buffer[send_index++] = id_ret;
				m_send_buffer[send_index++] = packet_id;
				commands_send_packet(m_send_buffer, send_index);
			}
		} break;

		case CMD_SET_MAIN_CONFIG: {
			commands_set_send_func(func);

			int32_t ind = 0;
			main_config.mag_use = data[ind++];
			main_config.mag_comp = data[ind++];
			main_config.yaw_mag_gain = buffer_get_float32_auto(data, &ind);

			main_config.mag_cal_cx = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_cy = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_cz = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_xx = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_xy = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_xz = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_yx = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_yy = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_yz = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_zx = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_zy = buffer_get_float32_auto(data, &ind);
			main_config.mag_cal_zz = buffer_get_float32_auto(data, &ind);

			main_config.gps_ant_x = buffer_get_float32_auto(data, &ind);
			main_config.gps_ant_y = buffer_get_float32_auto(data, &ind);
			main_config.gps_comp = data[ind++];
			main_config.gps_req_rtk = data[ind++];
			main_config.gps_use_rtcm_base_as_enu_ref = data[ind++];
			main_config.gps_corr_gain_stat = buffer_get_float32_auto(data, &ind);
			main_config.gps_corr_gain_dyn = buffer_get_float32_auto(data, &ind);
			main_config.gps_corr_gain_yaw = buffer_get_float32_auto(data, &ind);
			main_config.gps_send_nmea = data[ind++];
			main_config.gps_use_ubx_info = data[ind++];
			main_config.gps_ubx_max_acc = buffer_get_float32_auto(data, &ind);

			main_config.uwb_max_corr = buffer_get_float32_auto(data, &ind);

			main_config.ap_repeat_routes = data[ind++];
			main_config.ap_base_rad = buffer_get_float32_auto(data, &ind);
			main_config.ap_rad_time_ahead = buffer_get_float32_auto(data, &ind);
			main_config.ap_mode_time = data[ind++];
			main_config.ap_max_speed = buffer_get_float32_auto(data, &ind);
			main_config.ap_time_add_repeat_ms = buffer_get_int32(data, &ind);

			main_config.log_rate_hz = buffer_get_int16(data, &ind);
			main_config.log_en = data[ind++];
			strcpy(main_config.log_name, (const char*)(data + ind));
			ind += strlen(main_config.log_name) + 1;
			main_config.log_mode_ext = data[ind++];
			main_config.log_uart_baud = buffer_get_uint32(data, &ind);

			log_set_rate(main_config.log_rate_hz);
			log_set_enabled(main_config.log_en);
			log_set_name(main_config.log_name);

			// Car settings
			main_config.car.yaw_use_odometry = data[ind++];
			main_config.car.yaw_imu_gain = buffer_get_float32_auto(data, &ind);
			main_config.car.disable_motor = data[ind++];
			main_config.car.simulate_motor = data[ind++];
			main_config.car.clamp_imu_yaw_stationary = data[ind++];
			main_config.car.use_uwb_pos = data[ind++];

			main_config.car.gear_ratio = buffer_get_float32_auto(data, &ind);
			main_config.car.wheel_diam = buffer_get_float32_auto(data, &ind);
			main_config.car.motor_poles = buffer_get_float32_auto(data, &ind);
			main_config.car.steering_max_angle_rad = buffer_get_float32_auto(data, &ind);
			main_config.car.steering_center = buffer_get_float32_auto(data, &ind);
			main_config.car.steering_range = buffer_get_float32_auto(data, &ind);
			main_config.car.steering_ramp_time = buffer_get_float32_auto(data, &ind);
			main_config.car.axis_distance = buffer_get_float32_auto(data, &ind);

			motor_sim_set_running(main_config.car.simulate_motor);

			// Multirotor settings
			main_config.mr.vel_decay_e = buffer_get_float32_auto(data, &ind);
			main_config.mr.vel_decay_l = buffer_get_float32_auto(data, &ind);
			main_config.mr.vel_max = buffer_get_float32_auto(data, &ind);
			main_config.mr.map_min_x = buffer_get_float32_auto(data, &ind);
			main_config.mr.map_max_x = buffer_get_float32_auto(data, &ind);
			main_config.mr.map_min_y = buffer_get_float32_auto(data, &ind);
			main_config.mr.map_max_y = buffer_get_float32_auto(data, &ind);

			main_config.mr.vel_gain_p = buffer_get_float32_auto(data, &ind);
			main_config.mr.vel_gain_i = buffer_get_float32_auto(data, &ind);
			main_config.mr.vel_gain_d = buffer_get_float32_auto(data, &ind);

			main_config.mr.tilt_gain_p = buffer_get_float32_auto(data, &ind);
			main_config.mr.tilt_gain_i = buffer_get_float32_auto(data, &ind);
			main_config.mr.tilt_gain_d = buffer_get_float32_auto(data, &ind);

			main_config.mr.max_corr_error = buffer_get_float32_auto(data, &ind);
			main_config.mr.max_tilt_error = buffer_get_float32_auto(data, &ind);

			main_config.mr.ctrl_gain_roll_p = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_roll_i = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_roll_dp = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_roll_de = buffer_get_float32_auto(data, &ind);

			main_config.mr.ctrl_gain_pitch_p = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_pitch_i = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_pitch_dp = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_pitch_de = buffer_get_float32_auto(data, &ind);

			main_config.mr.ctrl_gain_yaw_p = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_yaw_i = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_yaw_dp = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_yaw_de = buffer_get_float32_auto(data, &ind);

			main_config.mr.ctrl_gain_pos_p = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_pos_i = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_pos_d = buffer_get_float32_auto(data, &ind);

			main_config.mr.ctrl_gain_alt_p = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_alt_i = buffer_get_float32_auto(data, &ind);
			main_config.mr.ctrl_gain_alt_d = buffer_get_float32_auto(data, &ind);

			main_config.mr.js_gain_tilt = buffer_get_float32_auto(data, &ind);
			main_config.mr.js_gain_yaw = buffer_get_float32_auto(data, &ind);
			main_config.mr.js_mode_rate = data[ind++];

			main_config.mr.motor_fl_f = data[ind++];
			main_config.mr.motor_bl_l = data[ind++];
			main_config.mr.motor_fr_r = data[ind++];
			main_config.mr.motor_br_b = data[ind++];
			main_config.mr.motors_x = data[ind++];
			main_config.mr.motors_cw = data[ind++];
			main_config.mr.motor_pwm_min_us = buffer_get_uint16(data, &ind);
			main_config.mr.motor_pwm_max_us = buffer_get_uint16(data, &ind);

			conf_general_store_main_config(&main_config);

			// Doing this while driving will get wrong as there is so much accelerometer noise then.
			//pos_reset_attitude();

			// Send ack
			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;
			commands_send_packet(m_send_buffer, send_index);
		} break;

		case CMD_GET_MAIN_CONFIG:
		case CMD_GET_MAIN_CONFIG_DEFAULT: {
			commands_set_send_func(func);

			MAIN_CONFIG main_cfg_tmp;

			if (packet_id == CMD_GET_MAIN_CONFIG) {
				main_cfg_tmp = main_config;
			} else {
				conf_general_get_default_main_config(&main_cfg_tmp);
			}

			int32_t send_index = 0;
			m_send_buffer[send_index++] = id_ret;
			m_send_buffer[send_index++] = packet_id;

			m_send_buffer[send_index++] = main_cfg_tmp.mag_use;
			m_send_buffer[send_index++] = main_cfg_tmp.mag_comp;
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.yaw_mag_gain, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_cx, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_cy, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_cz, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_xx, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_xy, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_xz, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_yx, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_yy, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_yz, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_zx, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_zy, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mag_cal_zz, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.gps_ant_x, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.gps_ant_y, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.gps_comp;
			m_send_buffer[send_index++] = main_cfg_tmp.gps_req_rtk;
			m_send_buffer[send_index++] = main_cfg_tmp.gps_use_rtcm_base_as_enu_ref;
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.gps_corr_gain_stat, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.gps_corr_gain_dyn, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.gps_corr_gain_yaw, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.gps_send_nmea;
			m_send_buffer[send_index++] = main_cfg_tmp.gps_use_ubx_info;
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.gps_ubx_max_acc, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.uwb_max_corr, &send_index);

			m_send_buffer[send_index++] = main_cfg_tmp.ap_repeat_routes;
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.ap_base_rad, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.ap_rad_time_ahead, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.ap_mode_time;
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.ap_max_speed, &send_index);
			buffer_append_int32(m_send_buffer, main_cfg_tmp.ap_time_add_repeat_ms, &send_index);

			buffer_append_int16(m_send_buffer, main_cfg_tmp.log_rate_hz, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.log_en;
			strcpy((char*)(m_send_buffer + send_index), main_cfg_tmp.log_name);
			send_index += strlen(main_config.log_name) + 1;
			m_send_buffer[send_index++] = main_cfg_tmp.log_mode_ext;
			buffer_append_uint32(m_send_buffer, main_cfg_tmp.log_uart_baud, &send_index);

			// Car settings
			m_send_buffer[send_index++] = main_cfg_tmp.car.yaw_use_odometry;
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.yaw_imu_gain, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.car.disable_motor;
			m_send_buffer[send_index++] = main_cfg_tmp.car.simulate_motor;
			m_send_buffer[send_index++] = main_cfg_tmp.car.clamp_imu_yaw_stationary;
			m_send_buffer[send_index++] = main_cfg_tmp.car.use_uwb_pos;

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.gear_ratio, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.wheel_diam, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.motor_poles, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.steering_max_angle_rad, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.steering_center, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.steering_range, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.steering_ramp_time, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.car.axis_distance, &send_index);

			// Multirotor settings
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.vel_decay_e, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.vel_decay_l, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.vel_max, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.map_min_x, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.map_max_x, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.map_min_y, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.map_max_y, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.vel_gain_p, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.vel_gain_i, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.vel_gain_d, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.tilt_gain_p, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.tilt_gain_i, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.tilt_gain_d, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.max_corr_error, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.max_tilt_error, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_roll_p, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_roll_i, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_roll_dp, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_roll_de, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_pitch_p, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_pitch_i, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_pitch_dp, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_pitch_de, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_yaw_p, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_yaw_i, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_yaw_dp, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_yaw_de, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_pos_p, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_pos_i, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_pos_d, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_alt_p, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_alt_i, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.ctrl_gain_alt_d, &send_index);

			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.js_gain_tilt, &send_index);
			buffer_append_float32_auto(m_send_buffer, main_cfg_tmp.mr.js_gain_yaw, &send_index);
			m_send_buffer[send_index++] = main_cfg_tmp.mr.js_mode_rate;

			m_send_buffer[send_index++] = main_cfg_tmp.mr.motor_fl_f;
			m_send_buffer[send_index++] = main_cfg_tmp.mr.motor_bl_l;
			m_send_buffer[send_index++] = main_cfg_tmp.mr.motor_fr_r;
			m_send_buffer[send_index++] = main_cfg_tmp.mr.motor_br_b;
			m_send_buffer[send_index++] = main_cfg_tmp.mr.motors_x;
			m_send_buffer[send_index++] = main_cfg_tmp.mr.motors_cw;
			buffer_append_uint16(m_send_buffer, main_cfg_tmp.mr.motor_pwm_min_us, &send_index);
			buffer_append_uint16(m_send_buffer, main_cfg_tmp.mr.motor_pwm_max_us, &send_index);

			commands_send_packet(m_send_buffer, send_index);
		} break;

		default:
			break;
		}

		// process vehicle-type-specific commands
		commands_specific_process_packet(packet_id, data, len, id_ret, func, m_send_buffer);
	}
}

void commands_printf(const char* format, ...) {
//	if (!m_init_done) {
//		return;
//	}

	//chMtxLock(&m_print_gps);
	va_list arg;
	va_start (arg, format);
	commands_vprintf(format, arg);
	va_end (arg);
	//chMtxUnlock(&m_print_gps);
}

void commands_vprintf(const char* format, va_list args) {
	int len;
	static char print_buffer[512];

	print_buffer[0] = main_id;
	print_buffer[1] = CMD_PRINTF;
	len = vsnprintf(print_buffer + 2, 509, format, args);

	if(len > 0) {
		commands_send_packet((unsigned char*)print_buffer, (len<509) ? len + 2: 512);
	}
}

#define LOG_LINE_SIZE 512
void commands_printf_log_serial(char* format, ...) {
	va_list arg;
	va_start (arg, format);
	int len;
	static char print_buffer[LOG_LINE_SIZE];

	print_buffer[0] = ID_CAR_CLIENT;
	print_buffer[1] = CMD_LOG_LINE_USB;
	len = vsnprintf(print_buffer + 2, LOG_LINE_SIZE-2, format, arg);
	va_end (arg);

	if(len > 0) {
		comm_serial_send_packet((unsigned char*)print_buffer, (len<LOG_LINE_SIZE-2) ? len + 2: LOG_LINE_SIZE);
	}
}

void commands_send_nmea(const char *data, unsigned int len) {
	if (main_config.gps_send_nmea) {
		int32_t send_index = 0;
		m_send_buffer[send_index++] = main_id;
		m_send_buffer[send_index++] = CMD_SEND_NMEA_RADIO;
		memcpy(m_send_buffer + send_index, data, len);
		send_index += len;
		commands_send_packet(m_send_buffer, send_index);
	}
}

void commands_init_plot(char *namex, char *namey) {
	int ind = 0;
	m_send_buffer[ind++] = main_id;
	m_send_buffer[ind++] = CMD_PLOT_INIT;
	memcpy(m_send_buffer + ind, namex, strlen(namex));
	ind += strlen(namex);
	m_send_buffer[ind++] = '\0';
	memcpy(m_send_buffer + ind, namey, strlen(namey));
	ind += strlen(namey);
	m_send_buffer[ind++] = '\0';
	commands_send_packet((unsigned char*)m_send_buffer, ind);
}

void commands_plot_add_graph(char *name) {
	int ind = 0;
	m_send_buffer[ind++] = main_id;
	m_send_buffer[ind++] = CMD_PLOT_ADD_GRAPH;
	memcpy(m_send_buffer + ind, name, strlen(name));
	ind += strlen(name);
	m_send_buffer[ind++] = '\0';
	commands_send_packet((unsigned char*)m_send_buffer, ind);
}

void commands_plot_set_graph(int graph) {
	int ind = 0;
	m_send_buffer[ind++] = main_id;
	m_send_buffer[ind++] = CMD_PLOT_SET_GRAPH;
	m_send_buffer[ind++] = graph;
	commands_send_packet((unsigned char*)m_send_buffer, ind);
}

void commands_send_plot_points(float x, float y) {
	int32_t ind = 0;
	m_send_buffer[ind++] = main_id;
	m_send_buffer[ind++] = CMD_PLOT_DATA;
	buffer_append_float32_auto(m_send_buffer, x, &ind);
	buffer_append_float32_auto(m_send_buffer, y, &ind);
	commands_send_packet((unsigned char*)m_send_buffer, ind);
}
