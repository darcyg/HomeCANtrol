#include <sys/time.h>
#define NAMELEN 255
#define MAX_ADD_PER_NODE 10
#define MAX_WSLEDS 20

typedef enum {
  WS_UNDEF = 0,
  WS_SEND_STATUS = 1,
  WS_SEND_ALL_STATUS = 2
} tWsSend ;

typedef enum {
  S_NOP = 0,
  S_DIM = 1,
  S_GOTO = 2,
  S_DELAY = 3,
} tSeqCom ;


typedef enum {
  N_UNDEF = 0,
  N_STRUCTURE = 1,
  N_ADRESS = 2,
  N_ONOFF = 3,
  N_SHADE = 4,
  N_SENSOR = 5,
  N_ACTION = 6,
  N_MACRO = 7,
  N_DELAY = 8,
  N_TIMER = 9,
  N_CALL = 10,
  N_TASK = 11,
  N_ALWAYS = 12,
  N_ACTIVE = 13,
  N_STARTUP = 14,
  N_IF = 15,
  N_VAR = 16,
  N_SET = 17,
  N_REPEAT = 18,
  N_LANGUAGE = 19,
  N_PORT = 20,
  N_BROADCAST = 21,
  N_FIRMWARE = 22,
  N_BAD = 23,
  N_PROGRAM = 24,
  N_SEQUENCE = 25,
  S_SIMPLE = 100,
  S_SHORTLONG = 101,
  S_SHADE_SHORTLONG = 102,
  S_SHADE_SIMPLE = 103,
  S_MONO = 104,
  S_RETMONO = 105,
  S_ANALOG = 106,
  S_OUTPUT = 107,
  S_WSDATA = 108,
  S_WSCLOCK = 109,
  S_BWM = 110,
  A_ON = 200,
  A_OFF = 201,
  A_TOGGLE = 202,
  A_SHADE_UP_FULL = 203,
  A_SHADE_DOWN_FULL = 204,
  A_SHADE_UP_SHORT = 205,
  A_SHADE_DOWN_SHORT = 206,
  A_SHADE_TO = 207,
  A_SEND_VAL = 208,
  A_HEARTBEAT = 209,
  A_CALL = 210,
  A_SEQUENCE = 211
} NodeType ;

typedef enum {
  W_MACRO=1,
  W_DELAY=2,
  W_TIME=3
} WaitType ;


struct TypSel {
  char *Name ;
  NodeType Type ;
} ;

struct AdInfo {
  int Linie ;
  int Knoten ;
  int Port ;
} ;

struct Aktion {
  int Type ;
  int StandAlone ;
  int Short ;
  char UnitName[NAMELEN*4] ;
  struct Node *Unit ;
  char Sequence[NAMELEN] ;
} ;

struct Werte {
  char UnitName[NAMELEN*4] ;
  int Wert ;
  int Vergleich ;
} ;

struct Program {
  unsigned char Port ;
  unsigned char Data[50] ;
} ;

struct Sensor {
  NodeType SensorTyp ;
  int Lang ;
  int Ende ;
  int Reset ;
  int Intervall ;
} ;

struct Rollo {
  int Lang ;
  int Kurz ;
  int Swap ;
} ;

struct Node {
  struct Node *Parent ;
  struct Node *Next ;
  struct Node *Prev ;
  struct Node *Child ;
  
  NodeType Type ;
  char TypeDef[NAMELEN] ;
  char Name[NAMELEN] ;
  int Value ;
  union {
    struct Sensor Sensor ;
    struct Rollo Rollo ;
    struct AdInfo Adresse ;
    struct Aktion Aktion ;
    struct Node *MakroStep ;
    struct timeval Time ;
    struct Program Program ;
    char UnitName[NAMELEN*4] ;
    char PAD[NAMELEN*5] ;
    struct Werte Wert ;
  } Data ;
} ;

struct ListItem {
  struct ListItem *Next ;
  struct ListItem *Prev ;
  int Number ;
  int Counter ;
  char Linie ;
  unsigned char Package ;
  char State ;
  unsigned short Knoten ;
  union {
    unsigned char Command[NAMELEN*4] ;
    struct EEPROM EEprom ;
    unsigned char *Code ;
  } Data ;
} ;

struct MacroList {
  struct Node *Macro ;
  WaitType DelayType ;
  union {
    struct Node *WaitNode ;
    struct timeval WaitTime ;
  } Delay ;
} ;

struct Sequence {
  struct Sequence *Next ;
  int LineNumber ;
  tSeqCom Command ;
  int Para;
  int CurrVal ;
  int DataLen ;
  unsigned char Data[MAX_WSLEDS*3] ;
} ;

struct SeqList {
  struct SeqList *Next ;
  struct Sequence *First ;
  struct Sequence *Current;
  struct Node *Action ;
  char Name[NAMELEN] ;
} ;

extern struct Node *Haus ;
extern struct SeqList *Sequences ;
// Node.c-Definitionen
struct Node *CreateNode (void);
void FreeNode (struct Node *This) ;
struct Node *NewChild (struct Node *This) ;
struct Node *FindNode (struct Node *Root,const char *Unit);
void FullObjectName(struct Node *Node, char *Name) ;
int CollectAdress (struct Node *Root, int Linie, int Knoten, struct Node *Result[], int *ResultNumber ) ;
int CollectType (struct Node *Root, NodeType Type, struct Node *Result[], int *ResultNumber ) ;
struct Node *FindNodeAdress (struct Node *Root,int Linie, int Knoten, int Port,struct Node *Except);
int GetNodeAdress (struct Node *Node, int *Line, int *Knoten, int *Port) ;
void FreeItem (struct ListItem *This) ;
struct ListItem *CreateItem (struct ListItem* Head) ;

// ParseXML.c-Definitionen
int ReadConfig(void) ;
void ReadSequence (char *Name, char *FileName) ;

// Server.c-Definitionen
void ExecuteMakro (struct Node *Makro);
void ExecuteSeq (struct Node *Action) ;
int HandleCommand (char *Command, char *Answer,int Socket) ;
