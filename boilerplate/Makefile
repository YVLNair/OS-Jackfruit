obj-m += monitor.o

all:
	gcc engine.c -o engine
	gcc cpu_hog.c -o cpu_hog
	gcc memory_hog.c -o memory_hog

kernel:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f engine cpu_hog memory_hog
