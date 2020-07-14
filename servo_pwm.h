/*
	Copyright 2017 Benjamin Vedder	benjamin@vedder.se

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

#ifndef SERVO_PWM_H_
#define SERVO_PWM_H_

#include <stdint.h>

// Functions
void servo_pwm_init(uint8_t servo_enable_mask);
void servo_pwm_set(uint8_t id, float pulse_width);
void servo_pwm_set_all(float pulse_width);

#endif /* SERVO_PWM_H_ */
