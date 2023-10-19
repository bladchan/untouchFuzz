#include <malloc.h>
#define MAX_VERTEX_NUM 10000
#define MAX_LOOP_CNT 50

#define AFL_BONUS_DEBUG

typedef struct ArcNode {

	int adjvex;
	struct ArcNode* nextarc;

}Edge;

typedef struct VNode {

	BasicBlock* bb;
	std::string bb_name;

	unsigned int cur_loc;

	int indegree;
	int outdegree;

	uint8_t visited;

	unsigned int bonus;
	unsigned int calledFunNum;

	Edge* firstarc;

	int loop_pre_bbs[MAX_LOOP_CNT];
	unsigned int loop_cnt;

}BBNode;

typedef struct ALGraph {

	BBNode list[MAX_VERTEX_NUM];

	int bb_num, edge_num;

}CFGraph;


int search_bb(CFGraph *cfg, BasicBlock* BB) {
	
	for (int i = 0; i < cfg->bb_num; i++) {
		if (cfg->list[i].bb == BB) {
			return i;
		}
	}

	return -1;  // no find

}


int insert_bb(CFGraph* cfg, BasicBlock* BB, unsigned int cur_loc) {

	int bb_idx = search_bb(cfg, BB);
	if (bb_idx != -1) // exist bb!
		return bb_idx;

	// create BBNode
	bb_idx = cfg->bb_num++;
	if (bb_idx >= MAX_VERTEX_NUM) {
		errs() << "Too many BBs?!\n";
		exit(-1);
	}

	cfg->list[bb_idx].bb = BB;
	cfg->list[bb_idx].cur_loc = cur_loc;
	cfg->list[bb_idx].indegree = 0;
	cfg->list[bb_idx].outdegree = 0;
	cfg->list[bb_idx].visited = 0;
	cfg->list[bb_idx].bonus = 0;
	cfg->list[bb_idx].firstarc = NULL;
	cfg->list[bb_idx].loop_cnt = 0;
	cfg->list[bb_idx].calledFunNum = 0;

	return bb_idx;

}

bool insert_edge(CFGraph* cfg, BasicBlock* BB, int bb_idx, int suc_bb_idx) {

	Edge* arc = (Edge*)malloc(sizeof(Edge));
	arc->adjvex = suc_bb_idx;
	arc->nextarc = NULL;

	Edge* t = cfg->list[bb_idx].firstarc;
	Edge* last_cur;
	cfg->edge_num++;
	cfg->list[bb_idx].outdegree++;
	cfg->list[suc_bb_idx].indegree++;

	if (!t) {
		cfg->list[bb_idx].firstarc = arc;
		return true;
	}

	while (t) {
		if (t->adjvex == suc_bb_idx) {
			free(arc);
			cfg->edge_num--;
			cfg->list[bb_idx].outdegree--;
			cfg->list[suc_bb_idx].indegree++;
			return false;
		}
		last_cur = t;
		t = t->nextarc;
	}

	last_cur->nextarc = arc;
	return true;

}

void dfs(CFGraph* cfg, Edge* edge, int pre_bb_idx) {

	BBNode* bb = &(cfg->list[edge->adjvex]);

	if (bb->outdegree == 0 || bb->visited == 2) {
		return;
	}

	if (bb->visited) {
		// loop detected
		if (bb->loop_cnt >= MAX_LOOP_CNT) {
			return;
		}
		bb->loop_pre_bbs[bb->loop_cnt++] = pre_bb_idx;

		if (bb->loop_cnt >= MAX_LOOP_CNT) {
			outs() << "Too many looppppppps?!\n";
			// exit(-1);
		}

		return;

	}

	if (bb->visited == 0)
		bb->visited = 1;

	for (Edge* node = bb->firstarc; node != NULL; node = node->nextarc)
		dfs(cfg, node, edge->adjvex);

	bb->visited = 2;

}

void loop_detect(CFGraph* cfg) {

	if (!cfg->edge_num) return; // no need!

	for (Edge* node = cfg->list[0].firstarc; node != NULL; node = node->nextarc) {

		cfg->list[0].visited = 0;
		dfs(cfg, node, 0);
		cfg->list[0].visited = 2;

	}

	// complete loop detect :)
#ifdef AFL_BONUS_DEBUG

	for (int i = 0; i < cfg->bb_num; i++) {

		BBNode* bb = &cfg->list[i];

		outs() << "BB: " << bb->bb_name << " (No." << i << ")\n";
		outs() << "		indegree: " << bb->indegree << "\n";
		outs() << "		loop: " << bb->loop_cnt << "\n";
		for (int j = 0; j < bb->loop_cnt; j++) {
			outs() << "			" << "No." << j << ": " << cfg->list[bb->loop_pre_bbs[j]].bb_name << "\n";
		}
		outs() << "		CalledFunNum: " << bb->calledFunNum << "\n";
		outs() << "************************************************************************" << "\n";

	}


#endif // AFL_BONUS_DEBUG


}