################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../control_motor.c \
../control_socket.c \
../data_socket.c \
../data_writer.c \
../dma_sdram.c \
../main.c 

OBJS += \
./control_motor.o \
./control_socket.o \
./data_socket.o \
./data_writer.o \
./dma_sdram.o \
./main.o 

C_DEPS += \
./control_motor.d \
./control_socket.d \
./data_socket.d \
./data_writer.d \
./dma_sdram.d \
./main.d 


# Each subdirectory must supply rules for building sources it contributes
%.o: ../%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler 4 [arm-linux-gnueabihf]'
	arm-linux-gnueabihf-gcc -IC:/intelFPGA/17.1/embedded/ip/altera/hps/altera_hps/hwlib/include -I"C:\intelFPGA\17.1\embedded\ip\altera\hps\altera_hps\hwlib\include\soc_cv_av" -O0 -g -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


