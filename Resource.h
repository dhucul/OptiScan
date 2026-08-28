//{{NO_DEPENDENCIES}}
// Microsoft Visual C++ generated include file.
// Used by OptiScan.rc

#define IDS_APP_TITLE			103

#define IDR_MAINFRAME			128
#define IDD_OPTISCAN_DIALOG	102
#define IDD_ABOUTBOX			103
#define IDM_ABOUT				104
#define IDM_EXIT				105
#define IDM_TOGGLE_ACCESSIBLE	106
// Theme selector menu items. Kept contiguous and in ThemeId order so
// (IDM_THEME_GRAPHITE + (int)ThemeId) maps id -> command. GRAPHITE..APPLELIGHT
// is also the CheckMenuRadioItem range, so anything that is not a theme pick
// must sort outside it.
#define IDM_THEME_GRAPHITE		110
#define IDM_THEME_CATPPUCCIN	111
#define IDM_THEME_NORD			112
#define IDM_THEME_ARCDARK		113
#define IDM_THEME_APPLELIGHT	114
// Not a theme pick and not part of the radio group -- clears the saved choice.
#define IDM_THEME_RESET			115
// Menu-click sound selector. A reserved contiguous command-id range: the
// "Click sound" submenu is built dynamically from UiSound::ClickStyleTable()
// (see OptiScan.cpp), and item i gets command id (IDM_SOUND_FIRST + i), which
// also indexes ClickStyle. The whole range must sort outside the theme
// CheckMenuRadioItem range (110-114) above. 24 slots reserved; 6 used today.
#define IDM_SOUND_FIRST			116
#define IDM_SOUND_LAST			139
#define IDI_OPTISCAN   		107
#define IDI_SMALL				108
#define IDC_OPTISCAN   		109
#define IDC_INFO_EDIT           1000
#define IDC_INFO_BUTTON1        1001
#define IDC_INFO_BUTTON2        1002
#define IDC_INFO_BUTTON3        1003
#define IDC_INFO_BUTTON4        1004
#define IDC_INFO_BUTTON5        1005
#define IDC_INFO_BUTTON6        1006
#define IDC_INFO_BUTTON7        1007
#define IDC_INFO_BUTTON8        1008
#define IDC_INFO_BUTTON9        1009
#define IDC_INFO_BUTTON10       1010
#define IDC_INFO_BUTTON11       1011
#define IDC_INFO_BUTTON12       1012
#define IDC_INFO_BUTTON13       1013
#define IDC_INFO_BUTTON14       1014
#define IDC_INFO_BUTTON15       1015
#define IDC_INFO_BUTTON16       1016
#define IDC_INFO_BUTTON17       1017
#define IDC_INFO_BUTTON18       1018
#define IDC_INFO_BUTTON19       1019
#define IDC_INFO_BUTTON20       1020
#define IDC_INFO_BUTTON21       1021
#define IDC_INFO_BUTTON22       1022
#define IDC_INFO_BUTTON23       1023
#define IDC_INFO_BUTTON24       1024
#define IDC_INFO_BUTTON25       1025
#define IDC_INFO_BUTTON26       1026
#define IDC_INFO_BUTTON27       1027
#define IDC_INFO_BUTTON28       1028
#define IDC_INFO_BUTTON29       1029
#define IDC_INFO_BUTTON30       1030
#define IDC_INFO_BUTTON31       1031
#define IDC_INFO_BUTTON32       1032
#define IDC_INFO_BUTTON33       1033
#define IDC_INFO_BUTTON34       1034
#define IDC_INFO_BUTTON35       1035
#define IDC_INFO_BUTTON36       1036
#define IDC_PROGRESS_TEXT       1040
#define IDC_PROGRESS_BAR        1041
#define IDC_ACCESSIBLE_EDIT     1042
#define IDC_DISC_INFO_LABEL     1101
#define IDC_DRIVE_LABEL         1102
#define IDC_UTILITY_LABEL       1103
#define IDC_ACCESSIBLE_LABEL    1104
#define IDC_MYICON				2
#ifndef IDC_STATIC
#define IDC_STATIC				-1
#endif
// Next default values for new objects
//
#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS

#define _APS_NO_MFC					130
#define _APS_NEXT_RESOURCE_VALUE	129
#define _APS_NEXT_COMMAND_VALUE		32771
#define _APS_NEXT_CONTROL_VALUE		1104
#define _APS_NEXT_SYMED_VALUE		110
#endif
#endif
