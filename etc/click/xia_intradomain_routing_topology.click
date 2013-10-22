require(library xia_router_lib.click);
require(library xia_address.click);

// host & router instantiation
//host0 :: XIAEndHost (RE AD0 HID0, HID0, 1500, 0, aa:aa:aa:aa:aa:aa);

router0 :: XIARouter2Port(RE AD0 RHID0, AD0, RHID0, 0.0.0.0, 1500, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa);
router1 :: XIARouter2Port(RE AD0 RHID1, AD0, RHID1, 0.0.0.0, 1600, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa);
controller0 :: XIAController(RE AD0 CHID0, AD0, CHID0, 0.0.0.0, 1700, aa:aa:aa:aa:aa:aa);

router2 :: XIARouter2Port(RE AD2 RHID2, AD2, RHID2, 0.0.0.0, 1800, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa);
controller2 :: XIAController(RE AD2 CHID2, AD2, CHID2, 0.0.0.0, 1900, aa:aa:aa:aa:aa:aa);


// The following line is required by the xianet script so it can determine the appropriate
// host/router pair to run the nameserver on
// host0 :: nameserver

// interconnection -- host - ad
controller0[0] -> LinkUnqueue(0.005, 1 GB/s) -> [0]router0;
router0[0] -> LinkUnqueue(0.005, 1 GB/s) -> [0]controller0;

// interconnection -- ad - ad
router0[1] -> LinkUnqueue(0.005, 1 GB/s) ->[1]router1;
router1[1] -> LinkUnqueue(0.005, 1 GB/s) ->[1]router0;

router2[1] -> LinkUnqueue(0.005, 1 GB/s) -> [0]router1;
router1[0] -> LinkUnqueue(0.005, 1 GB/s) ->[1]router2;

router2[0] -> LinkUnqueue(0.005, 1 GB/s) -> [0]controller2;
controller2[0] -> LinkUnqueue(0.005, 1 GB/s) ->[0]router2;

ControlSocket(tcp, 7777);