#pragma once

#include "electricmaple.pb.h"

typedef struct _em_UpMessageSuper
{
	em_proto_UpMessage protoMessage;

	em_proto_HandJointLocation hand_joint_locations_left[26];
	em_proto_HandJointLocation hand_joint_locations_right[26];
} em_UpMessageSuper;
