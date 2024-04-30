#!/bin/bash

echo -e "\nDetecting the virtual bios version:"

num=0
while read line
do
	file_array[$num]="$line"
	((num++))
done < <(find ./ -type f -name "*uefi-bios*" && find /usr/local/share/qemu/ -type f -name "*uefi-bios*" && find /usr/share/qemu/ -type f -name "*uefi-bios*")


echo -e "\nBIOS information:"

for ((i=0; i<num; i++))
do
	if [ -n ${file_array[$i]} ];then
		echo -e "filename: \033[34m${file_array[$i]}\033[0m"
		firm_version=$(hexdump -C -v -s 0x3ff00 -n 6 ${file_array[$i]} | cut -c 14-27 )
		array2=($firm_version)
		if [[ $[$((16#${array2[4]}))/10] == 10 && $[$((16#${array2[3]}))/10] == 5 ]];then
			echo -e "version: \033[31mv$((16#${array2[4]}))"."$((16#${array2[3]}))"."$((16#${array2[2]}))"."$((16#${array2[1]}))"."$((16#${array2[0]}))\033[0m\n"
		else
			echo -e "version: \033[31mUnknown version!\033[0m Under 2.1.0!\n"
		fi
	fi
done
