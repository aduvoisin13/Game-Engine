#include "AnimComponent.h"
#include "../../ticpp/ticpp.h"
#include <sstream>
#include <d3d9.h>
#include <d3dx9math.h>

namespace ITP485
{

AnimComponent::AnimComponent( const char* szFileName )
: m_Palette(nullptr)
{
	Parse(szFileName);
	InitializeData();
}

void AnimComponent::InitializeData()
{
	// Error check.
	Dbg_Assert(m_Skeleton.m_iNumJoints > 0, "No joints in m_Skeleton!");

	// Calculate the inverse bind pose matrix for each joint.
	// For the first joint (root), we just invert local pose.
	m_Skeleton.m_pJoints[0].inv_bindPose = m_Skeleton.m_pJoints[0].localPose;
	// For every other joint, we multiply up the chain.
	for (short i = 1; i < m_Skeleton.m_iNumJoints; ++i)
	{
		m_Skeleton.m_pJoints[i].inv_bindPose = m_Skeleton.m_pJoints[m_Skeleton.m_pJoints[i].m_ParentIndex].inv_bindPose;
		m_Skeleton.m_pJoints[i].inv_bindPose.Multiply(m_Skeleton.m_pJoints[i].localPose);
	}
	// Do all inversions at the end.
	for (short i = 0; i < m_Skeleton.m_iNumJoints; ++i)
	{
		m_Skeleton.m_pJoints[i].inv_bindPose.Invert();
	}

	// Set up the initial pose.
	for (short i = 0; i < m_Skeleton.m_iNumJoints; ++i)
	{
		m_Pose.m_pPoses[i].localPose = m_CurrAnimation.m_pKeyFrames[i][0].localPose;
	}

	// Set up the matrix pallette.
	m_Palette[0] = m_Pose.m_pPoses[0].localPose;
	for (short i = 1; i < m_Skeleton.m_iNumJoints; ++i)
	{
		m_Palette[i] = m_Palette[m_Skeleton.m_pJoints[i].m_ParentIndex];
		m_Palette[i].Multiply(m_Pose.m_pPoses[i].localPose);
	}
	// Multiply by inverse bind pose at the end.
	for (short i = 0; i < m_Skeleton.m_iNumJoints; ++i)
	{
		m_Palette[i].Multiply(m_Skeleton.m_pJoints[i].inv_bindPose);
	}
}

AnimComponent::~AnimComponent()
{
	// Delete stuff so we don't leak copious amounts of memory
	delete[] m_Skeleton.m_pJoints;
	for (int i = 0; i < m_Skeleton.m_iNumJoints; i++)
	{
		KeyFrame* curr = m_CurrAnimation.m_pKeyFrames[i];
		while (curr != nullptr)
		{
			KeyFrame* next = curr->m_Next;
			delete curr;
			curr = next;
		}
	}
	delete[] m_CurrAnimation.m_pKeyFrames;

	_aligned_free(m_Palette);
}

void AnimComponent::CalculatePose(short joint, KeyFrame* frame1, KeyFrame* frame2)
{
	// Local current pose matrix for that joint.
	if (frame1 == frame2)
	{
		m_Pose.m_pPoses[joint].localPose = frame1->localPose;
	}
	else
	{
		// Interpolate between the two frame times.
		float time1 = float(frame1->m_FrameNum) / 24.0f;
		float time2;
		if (frame1->m_FrameNum > frame2->m_FrameNum)
		{
			// Loops back to frame 0.
			time2 = float(m_CurrAnimation.m_NumFrames) / 24.0f;
		}
		else
		{
			// Standard case.
			time2 = float(frame2->m_FrameNum) / 24.0f;
		}

		m_Pose.m_pPoses[joint].localPose = Lerp(
			frame1->localPose,
			frame2->localPose,
			((m_CurrAnimation.m_Time - time1) / (time2 - time1)));
	}

	// Update matrix palette with global current pose.
	if (m_Skeleton.m_pJoints[joint].m_ParentIndex == -1)
	{
		m_Palette[joint] = m_Pose.m_pPoses[joint].localPose;
	}
	else
	{
		m_Palette[joint] = m_Palette[m_Skeleton.m_pJoints[joint].m_ParentIndex];
		m_Palette[joint].Multiply(m_Pose.m_pPoses[joint].localPose);
	}

	// Multiply by inverse bind pose at the very end, after all joints are calculated.
}

void AnimComponent::Update( float fDelta )
{
	m_CurrAnimation.m_Time += fDelta;

	// Frame rate from Maya is 24.0 FPS.
	m_CurrAnimation.m_CurrFrame = int(24.0f * m_CurrAnimation.m_Time);

	// Loop the animation.
	if (m_CurrAnimation.m_CurrFrame >= m_CurrAnimation.m_NumFrames)
	{
		m_CurrAnimation.m_CurrFrame -= m_CurrAnimation.m_NumFrames;
		m_CurrAnimation.m_Time -= float(m_CurrAnimation.m_NumFrames) / 24.0f;
	}

	// Animate!
	for (short i = 0; i < m_Skeleton.m_iNumJoints; ++i)
	{
		KeyFrame* frame1 = m_CurrAnimation.m_pKeyFrames[i];
		KeyFrame* frame2 = m_CurrAnimation.m_pKeyFrames[i]->m_Next;

		while (frame2 != nullptr)
		{
			if (frame1->m_FrameNum <= m_CurrAnimation.m_CurrFrame
				&& frame2->m_FrameNum > m_CurrAnimation.m_CurrFrame)
			{
				CalculatePose(i, frame1, frame2);
				break;
			}

			frame1 = frame2;
			frame2 = frame2->m_Next;
		}

		if (frame2 == nullptr)
		{
			CalculatePose(i, frame1, m_CurrAnimation.m_pKeyFrames[i]);
		}
	}

	// Multiply matrix palette by each inverse bind pose.
	for (short i = 0; i < m_Skeleton.m_iNumJoints; ++i)
	{
		m_Palette[i].Multiply(m_Skeleton.m_pJoints[i].inv_bindPose);
	}
}

// Set the matrix palette.
void AnimComponent::StoreMatrixPalette( ID3DXEffect* pEffect )
{
	pEffect->SetMatrixArray("gPalette", static_cast<D3DXMATRIX*>(m_Palette->ToD3D()), 32);
}

void AnimComponent::Parse( const char* szFileName )
{
	// Parse the itpanim file.
	ticpp::Document doc(szFileName);
	doc.LoadFile();

	// Basic XML parsing code.
	ticpp::Iterator<ticpp::Element> child;
	for(child = child.begin(doc.FirstChildElement()); child != child.end(); child++)
	{
		// The value of this child identifies the name of this element
		std::string strName;
		std::string strValue;
		std::string strText;

		child->GetValue(&strName);
		if (strName == "skeleton")
		{
			// Initialize the bones array
			strValue = child->GetAttribute("count");
			m_Skeleton.m_iNumJoints = atoi(strValue.c_str());
			m_Skeleton.m_pJoints = new Joint[m_Skeleton.m_iNumJoints];

			// Skeleton pose
			m_Pose.m_pPoses = new JointPose[m_Skeleton.m_iNumJoints];

			// Matrix palette
			void* buf = _aligned_malloc(sizeof(Matrix4) * m_Skeleton.m_iNumJoints, 16);
			m_Palette = new (buf) Matrix4[m_Skeleton.m_iNumJoints];

			// Now get every joint
			ticpp::Iterator<ticpp::Element> joint;
			for(joint = joint.begin(child.Get()); joint != joint.end(); joint++)
			{
				float mat[4][4];

				strValue = joint->GetAttribute("id");
				int index = atoi(strValue.c_str());
				
				m_Skeleton.m_pJoints[index].m_Name = joint->GetAttribute("name");
				
				strValue = joint->GetAttribute("parent");
				int parent = atoi(strValue.c_str());
				m_Skeleton.m_pJoints[index].m_ParentIndex = parent;

				// Get the translation/quat for this joint
				ticpp::Iterator<ticpp::Element> ele;
				for(ele = ele.begin(joint.Get()); ele != ele.end(); ele++)
				{
					ele->GetValue(&strName);
					if (strName == "mat")
					{
						sscanf_s(ele->GetText().c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
							&mat[0][0], &mat[0][1], &mat[0][2], &mat[0][3],
							&mat[1][0], &mat[1][1], &mat[1][2], &mat[1][3],
							&mat[2][0], &mat[2][1], &mat[2][2], &mat[2][3],
							&mat[3][0], &mat[3][1], &mat[3][2], &mat[3][3]);
						m_Skeleton.m_pJoints[index].localPose.Set(mat);
					}
				}
			}
		}
		else if (strName == "animations")
		{
			// Now get every animation
			ticpp::Iterator<ticpp::Element> anim;
			for(anim = anim.begin(child.Get()); anim != anim.end(); anim++)
			{
				float mat[4][4];

				// Get Name and length
				strValue = anim->GetAttribute("name");
				m_CurrAnimation.m_Name = strValue;

				strValue = anim->GetAttribute("length");
				m_CurrAnimation.m_NumFrames = atoi(strValue.c_str());
				m_CurrAnimation.m_CurrFrame = 0;

				// Initialize key frame array of pointers
				m_CurrAnimation.m_pKeyFrames = new KeyFrame*[m_Skeleton.m_iNumJoints];

				// Now loop through the tracks
				ticpp::Iterator<ticpp::Element> track;
				for(track = track.begin(anim.Get()); track != track.end(); track++)
				{
					strValue = track->GetAttribute("id");
					int index = atoi(strValue.c_str());

					KeyFrame* PrevKey = nullptr;
					// Go through every key frame for this track
					ticpp::Iterator<ticpp::Element> key;
					for(key = key.begin(track.Get()); key != key.end(); key++)
					{
						// Instantiate this key frame
						KeyFrame* CurrKey = new KeyFrame;

						// Set previous key's ptr if one exists
						if (PrevKey != nullptr)
						{
							PrevKey->m_Next = CurrKey;
						}
						else // otherwise this one is the first
						{
							m_CurrAnimation.m_pKeyFrames[index] = CurrKey;
						}

						// Grab frame number
						strValue = key->GetAttribute("num");
						CurrKey->m_FrameNum = atoi(strValue.c_str());

						// Now grab the data for this key frame
						ticpp::Iterator<ticpp::Element> ele;
						for(ele = ele.begin(key.Get()); ele != ele.end(); ele++)
						{
							ele->GetValue(&strName);
							if (strName == "mat")
							{
								sscanf_s(ele->GetText().c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
									&mat[0][0], &mat[0][1], &mat[0][2], &mat[0][3],
									&mat[1][0], &mat[1][1], &mat[1][2], &mat[1][3],
									&mat[2][0], &mat[2][1], &mat[2][2], &mat[2][3],
									&mat[3][0], &mat[3][1], &mat[3][2], &mat[3][3]);
								CurrKey->localPose.Set(mat);
							}
						}

						PrevKey = CurrKey;
					}
				}
			}
		}
	}
}

} // end namespace
