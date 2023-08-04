#!/bin/bash

echo -e "\nDetecting the virtual bios version:"

FILE1=./pc-bios/uefi-bios-sw
FILE2=/usr/local/share/qemu/uefi-bios-sw
FILE3=/usr/share/qemu/uefi-bios-sw

file_array=("$FILE1" "$FILE2" "$FILE3")
array1=()

echo -e "\nBIOS information:"

for ((i=0; i<3; i++))
do
	if [ -f "${file_array[$i]}" ];then
		echo "${file_array[$i]} exist"
		array1[$i]=1
	else
		echo "${file_array[$i]} does not exist"
		array1[$i]=0
		echo ""
	fi

	if [ ${array1[$i]} -eq 1 ];then
		firm_version=$(hexdump -C -v -s 0x3ff00 -n 6 ${file_array[$i]} | cut -c 14-27 )
		array2=($firm_version)
		if [[ $[$((16#${array2[4]}))/10] == 10 && $[$((16#${array2[3]}))/10] == 5 ]];then
			echo version: v$((16#${array2[4]}))"."$((16#${array2[3]}))"."$((16#${array2[2]}))"."$((16#${array2[1]}))"."$((16#${array2[0]}))
		else
			echo version: Unknown version. Your uefi-bios-sw version is under 2.1.0 !
		fi
		echo -e "filename: ${file_array[$i]}\n"
	fi
done
