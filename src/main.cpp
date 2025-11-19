/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>

#include <zephyr/drivers/led.h>


#include <zephyr/devicetree.h>

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(green_led)

const struct led_dt_spec spec = LED_DT_SPEC_GET(DT_NODELABEL(green_led));
//const struct led_dt_spec spec = LED_DT_SPEC_GET(DT_ALIAS(green_led));
#define SLEEP_MSEC	25U


/*
	leds {
		compatible = "gpio-leds";
		green_led: led_1 {
			gpios = <&gpio0 27 GPIO_ACTIVE_LOW>;
			label = "Green LED";
			status = "disabled";
		};
		blue_led: led_2 {
			gpios = <&gpio1 2 GPIO_ACTIVE_LOW>;
			label = "Blue LED";
			status = "disabled";
		};
		red_led: led_3 {
			gpios = <&gpio0 10 GPIO_ACTIVE_LOW>;
			label = "Red LED";
			status = "disabled";
		};
	};
*/
//we have multipe
//  

//
// DT_PROP(DT_ALIAS(green_led), status) // "okay"
// DT_PROP(DT_NODELABEL(led_1), status) // "okay"
// DT_PROP(DT_INST(0, gpio_leds) , status) // "disabled" just when is okay






int main(void)
{
	int err = 0;

	std::cout << "Hello, C++ world! " << CONFIG_BOARD << std::endl;
	//device_is_ready(spec->dev);
	if(!led_is_ready_dt(&spec))
	{
		std::cout<<"error init led \n";
		err = -ENODEV;
	}

	while(1){

		led_on_dt(&spec);
		k_sleep(K_MSEC(SLEEP_MSEC));
		led_off_dt(&spec);
		k_sleep(K_MSEC(SLEEP_MSEC));

	}
	return 0;
}
