
echo "PRE-OP"
cansend can0 000#8010
sleep .05

#cansend can0 000#8001
cansend can0 610#2f001a0000000000
sleep .05

#din 6000
cansend can0 610#23001a0108010060
sleep .05
cansend can0 610#23001a0208020060
sleep .05
#counter 2600
cansend can0 610#23001a0318010026
sleep .05
cansend can0 610#23001a0418020026
sleep .05

cansend can0 610#2f001a0004000000
sleep .05



#sub index1: disable pdo for allowing next settings
cansend can0 610#2300180100000080
sleep .05

#sub index2: transmission type: 255 is "COV"
cansend can0 610#2f001802ff000000
sleep .05

#sub index3:inhibit time = 50 *100us
cansend can0 610#2b00180332000000
sleep .05

##sub index5:event timer = 100 *1ms 
cansend can0 610#2b00180564000000
sleep .05

#sub index1: cob-id:190 set the pdo from invalid to valid
cansend can0 610#2300180190010000
sleep .05


echo "enable analog inputs sending"
cansend can0 610#2f23640001000000

echo "OPERATIONAL"
cansend can0 000#0110

sleep .05

echo "All scripts executed successfully!"