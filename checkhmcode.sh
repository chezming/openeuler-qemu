#!/bin/bash
echo `date +"============= %Y-%m-%d-%H:%M:%S ================"`
echo "We will check whether the hmcode-version of this machine is correct ..."
result=$(hexdump -C -v -s 130816 -n 48  /dev/mem | cut -c 62-77 | grep "Ver" | cut -c 12-16 )
fir=0
sec=0
thi=6
if [[ "$result" != "" ]]
then
	array=(${result//./ })
	if [ ${array[0]} -gt $fir ];then
		if [ ${array[1]} -gt $sec ];then
			if [ ${array[2]} -gt $thi ];then
				echo "Local hmcode version of physical machine is :"
				echo $result
				echo "version is ok ."
			else
				echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6
your local version is :"
				echo $result
				exit 1
			fi
		else
			echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6
your local version is :"
			echo $result
			exit 1
		fi
	else
		echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6
your local version is :"
		echo $result
		exit 1
	fi
else
	echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6"
	exit 1
fi
