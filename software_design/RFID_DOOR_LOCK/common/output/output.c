#include <stdio.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include "output.h"
void output_io_create(gpio_num_t gpio_num)
{
	gpio_pad_select_gpio(gpio_num);
	gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT); 
} 
void output_io_set_level(gpio_num_t gpio_num,int level) 
{ 
	gpio_set_level(gpio_num, level);
} 
