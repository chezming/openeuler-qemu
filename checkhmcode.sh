#!/bin/bash
echo `date +"============= %Y-%m-%d-%H:%M:%S ================"`
echo "We will check whether the hmcode-version of this machine is correct ..."
echo "If the hmcode version is too old, please refer to https://developer.wxiat.com/understand/ecologial/37
for the right hmcode version ..."
result=$(hexdump -C -v -s 130816 -n 4  /dev/mem | cut -c 14-21 )
fir=0
sec=0
thi=6
inc=1
if [[ "$result" != "" ]]
then
	array=(${result// / })
	if [ "" = "${array[2]}" ] || [ "" = "${array[1]}" ] || [ "" = "${array[0]}" ];then
                echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6
your local version is :"
                echo ${array[2]}"."${array[1]}"."${array[0]}
		exit 1
        fi
	if [ ${array[2]} -gt $fir ];then
		if [ ${array[2]} -gt $inc ];then
			echo "Local hmcode version of physical machine is :"
			echo ${array[2]}"."${array[1]}"."${array[0]}
			echo "version is ok ."
			exit 0
		fi
			if [ ${array[1]} -gt $sec ];then
				if [ ${array[1]} -gt $inc ];then
					echo "Local hmcode version of physical machine is :"
					echo ${array[2]}"."${array[1]}"."${array[0]}
					echo "version is ok ."
					exit 0
				fi
					if [ ${array[0]} -gt $thi ];then
						echo "Local hmcode version of physical machine is :"
						echo ${array[2]}"."${array[1]}"."${array[0]}
						echo "version is ok ."
						exit 0
					else
						echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6
your local version is :"
						echo ${array[2]}"."${array[1]}"."${array[0]}
						exit 1
					fi
				else
					echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6
your local version is :"
					echo ${array[2]}"."${array[1]}"."${array[0]}
					exit 1
				fi
			else
				echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6
your local version is :"
				echo ${array[2]}"."${array[1]}"."${array[0]}
				exit 1
			fi
		else
			echo "error: hmcode version is too old , please upgrade your hmcode first !
physical machine hmcode version must be higher than v1.1.6"
			exit 1
		fi
