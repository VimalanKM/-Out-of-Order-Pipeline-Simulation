//////////////////////////////////////////////////////////////////////
// In part B, you must modify this file to implement the following: //
// - void pipe_cycle_issue(Pipeline *p)                             //
// - void pipe_cycle_schedule(Pipeline *p)                          //
// - void pipe_cycle_writeback(Pipeline *p)                         //
// - void pipe_cycle_commit(Pipeline *p)                            //
//////////////////////////////////////////////////////////////////////

// pipeline.cpp
// Implements the out-of-order pipeline.

#include "pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * The width of the pipeline; that is, the maximum number of instructions that
 * can be processed during any given cycle in each of the issue, schedule, and
 * commit stages of the pipeline.
 * 
 * (Note that this does not apply to the writeback stage: as many as
 * MAX_WRITEBACKS instructions can be written back to the ROB in a single
 * cycle!)
 * 
 * When the width is 1, the pipeline is scalar.
 * When the width is greater than 1, the pipeline is superscalar.
 */
extern uint32_t PIPE_WIDTH;

/**
 * The number of entries in the ROB; that is, the maximum number of
 * instructions that can be stored in the ROB at any given time.
 * 
 * You should use only this many entries of the ROB::entries array.
 */
extern uint32_t NUM_ROB_ENTRIES;

/**
 * Whether to use in-order scheduling or out-of-order scheduling.
 * 
 * The possible values are SCHED_IN_ORDER for in-order scheduling and
 * SCHED_OUT_OF_ORDER for out-of-order scheduling.
 * 
 * Your implementation of pipe_cycle_sched() should check this value and
 * implement scheduling of instructions accordingly.
 */
extern SchedulingPolicy SCHED_POLICY;

/**
 * The number of cycles an LD instruction should take to execute.
 * 
 * This is used by the code in exeq.cpp to determine how long to wait before
 * considering the execution of an LD instruction done.
 */
extern uint32_t LOAD_EXE_CYCLES;

/**
 * Read a single trace record from the trace file and use it to populate the
 * given fe_latch.
 * 
 * You should not modify this function.
 * 
 * @param p the pipeline whose trace file should be read
 * @param fe_latch the PipelineLatch struct to populate
 */
void pipe_fetch_inst(Pipeline *p, PipelineLatch *fe_latch)
{
    InstInfo *inst = &fe_latch->inst;
    TraceRec trace_rec;
    uint8_t *trace_rec_buf = (uint8_t *)&trace_rec;
    size_t bytes_read_total = 0;
    ssize_t bytes_read_last = 0;
    size_t bytes_left = sizeof(TraceRec);

    // Read a total of sizeof(TraceRec) bytes from the trace file.
    while (bytes_left > 0)
    {
        bytes_read_last = read(p->trace_fd, trace_rec_buf, bytes_left);
        if (bytes_read_last <= 0)
        {
            // EOF or error
            break;
        }

        trace_rec_buf += bytes_read_last;
        bytes_read_total += bytes_read_last;
        bytes_left -= bytes_read_last;
    }

    // Check for error conditions.
    if (bytes_left > 0 || trace_rec.op_type >= NUM_OP_TYPES)
    {
        fe_latch->valid = false;
        p->halt_inst_num = p->last_inst_num;

        if (p->stat_retired_inst >= p->halt_inst_num)
        {
            p->halt = true;
        }

        if (bytes_read_last == -1)
        {
            fprintf(stderr, "\n");
            perror("Couldn't read from pipe");
            return;
        }

        if (bytes_read_total == 0)
        {
            // No more trace records to read
            return;
        }

        // Too few bytes read or invalid op_type
        fprintf(stderr, "\n");
        fprintf(stderr, "Error: Invalid trace file\n");
        return;
    }

    // Got a valid trace record!
    fe_latch->valid = true;
    fe_latch->stall = false;
    inst->inst_num = ++p->last_inst_num;
    inst->op_type = (OpType)trace_rec.op_type;

    inst->dest_reg = trace_rec.dest_needed ? trace_rec.dest_reg : -1;
    inst->src1_reg = trace_rec.src1_needed ? trace_rec.src1_reg : -1;
    inst->src2_reg = trace_rec.src2_needed ? trace_rec.src2_reg : -1;

    inst->dr_tag = -1;
    inst->src1_tag = -1;
    inst->src2_tag = -1;
    inst->src1_ready = false;
    inst->src2_ready = false;
    inst->exe_wait_cycles = 0;
}

/**
 * Allocate and initialize a new pipeline.
 * 
 * You should not need to modify this function.
 * 
 * @param trace_fd the file descriptor from which to read trace records
 * @return a pointer to a newly allocated pipeline
 */
Pipeline *pipe_init(int trace_fd)
{
    printf("\n** PIPELINE IS %d WIDE **\n\n", PIPE_WIDTH);

    // Allocate pipeline.
    Pipeline *p = (Pipeline *)calloc(1, sizeof(Pipeline));

    // Initialize pipeline.
    p->rat = rat_init();
    p->rob = rob_init();
    p->exeq = exeq_init();
    p->trace_fd = trace_fd;
    p->halt_inst_num = (uint64_t)(-1) - 3;

    for (unsigned int i = 0; i < PIPE_WIDTH; i++)
    {
        p->FE_latch[i].valid = false;
        p->ID_latch[i].valid = false;
        p->SC_latch[i].valid = false;
    }
    for (unsigned int i = 0; i < MAX_WRITEBACKS; i++)
    {
        p->EX_latch[i].valid = false;
    }

    return p;
}

/**
 * Commit the given instruction.
 * 
 * This updates counters and flags on the pipeline.
 * 
 * This function is implemented for you. You should not modify it.
 * 
 * @param p the pipeline to update.
 * @param inst the instruction to commit.
 */
void pipe_commit_inst(Pipeline *p, InstInfo inst)
{
    p->stat_retired_inst++;

    if (inst.inst_num >= p->halt_inst_num)
    {
        p->halt = true;
    }
}

/**
 * Print out the state of the pipeline for debugging purposes.
 * 
 * You may use this function to help debug your pipeline implementation, but
 * please remove calls to this function before submitting the lab.
 * 
 * @param p the pipeline
 */
void pipe_print_state(Pipeline *p)
{
    printf("\n");
    // Print table header
    for (unsigned int latch_type = 0; latch_type < 4; latch_type++)
    {
        switch (latch_type)
        {
        case 0:
            printf(" FE:    ");
            break;
        case 1:
            printf(" ID:    ");
            break;
        case 2:
            printf(" SCH:   ");
            break;
        case 3:
            printf(" EX:    ");
            break;
        default:
            printf(" ------ ");
        }
    }
    printf("\n");

    // Print row for each lane in pipeline width
    unsigned int ex_i = 0;
    for (unsigned int i = 0; i < PIPE_WIDTH; i++)
    {
        if (p->FE_latch[i].valid)
        {
            printf(" %6lu ",
                   (unsigned long)p->FE_latch[i].inst.inst_num);
        }
        else
        {
            printf(" ------ ");
        }
        if (p->ID_latch[i].valid)
        {
            printf(" %6lu ",
                   (unsigned long)p->ID_latch[i].inst.inst_num);
        }
        else
        {
            printf(" ------ ");
        }
        if (p->SC_latch[i].valid)
        {
            printf(" %6lu ",
                   (unsigned long)p->SC_latch[i].inst.inst_num);
        }
        else
        {
            printf(" ------ ");
        }
        bool flag = false;
        for (; ex_i < MAX_WRITEBACKS; ex_i++)
        {
            if (p->EX_latch[ex_i].valid)
            {
                printf(" %6lu ",
                       (unsigned long)p->EX_latch[ex_i].inst.inst_num);
                ex_i++;
                flag = true;
                break;
            }
        }
        if (!flag) {
            printf(" ------ ");
        }
        printf("\n");
    }
    printf("\n");

    rat_print_state(p->rat);
    exeq_print_state(p->exeq);
    rob_print_state(p->rob);
}

/**
 * Simulate one cycle of all stages of a pipeline.
 * 
 * You should not need to modify this function except for debugging purposes.
 * If you add code to print debug output in this function, remove it or comment
 * it out before you submit the lab.
 * 
 * @param p the pipeline to simulate
 */
void pipe_cycle(Pipeline *p)
{
    p->stat_num_cycle++;

    #ifdef DEBUG
        printf("\n--------------------------------------------\n");
        printf("Cycle count: %lu, retired instructions: %lu\n\n",
           (unsigned long)p->stat_num_cycle,
           (unsigned long)p->stat_retired_inst);
    #endif
    
    // In our simulator, stages are processed in reverse order.
    pipe_cycle_commit(p);
    pipe_cycle_writeback(p);
    pipe_cycle_exe(p);
    pipe_cycle_schedule(p);
    pipe_cycle_issue(p);
    pipe_cycle_decode(p);
    pipe_cycle_fetch(p);

    // Compile with "make debug" to have this show!
    #ifdef DEBUG
        pipe_print_state(p);
    #endif
}

/**
 * Simulate one cycle of the fetch stage of a pipeline.
 * 
 * This function is implemented for you. You should not modify it.
 * 
 * @param p the pipeline to simulate
 */
void pipe_cycle_fetch(Pipeline *p)
{
    for (unsigned int i = 0; i < PIPE_WIDTH; i++)
    {
        if (!p->FE_latch[i].stall && !p->FE_latch[i].valid)
        {
            // No stall and latch empty, so fetch a new instruction.
            pipe_fetch_inst(p, &p->FE_latch[i]);
        }
    }
}

/**
 * Simulate one cycle of the instruction decode stage of a pipeline.
 * 
 * This function is implemented for you. You should not modify it.
 * 
 * @param p the pipeline to simulate
 */
void pipe_cycle_decode(Pipeline *p)
{
    static uint64_t next_inst_num = 1;
    for (unsigned int i = 0; i < PIPE_WIDTH; i++)
    {
        if (!p->ID_latch[i].stall && !p->ID_latch[i].valid)
        {
            // No stall and latch empty, so decode the next instruction.
            // Loop to find the next in-order instruction.
            for (unsigned int j = 0; j < PIPE_WIDTH; j++)
            {
                if (p->FE_latch[j].valid &&
                    p->FE_latch[j].inst.inst_num == next_inst_num)
                {
                    p->ID_latch[i] = p->FE_latch[j];
                    p->FE_latch[j].valid = false;
                    next_inst_num++;
                    break;
                }
            }
        }
    }
}

/**
 * Simulate one cycle of the execute stage of a pipeline. This handles
 * instructions that take multiple cycles to execute.
 * 
 * This function is implemented for you. You should not modify it.
 * 
 * @param p the pipeline to simulate
 */
void pipe_cycle_exe(Pipeline *p)
{
    // If all operations are single-cycle, just copy SC latches to EX latches.
    /*#ifdef DEBUG
            printf("execution stage\n");
    #endif*/
    if (LOAD_EXE_CYCLES == 1)
    {
        /*#ifdef DEBUG
            printf("LOAD_EXE_CYCLES = 1\n");
        #endif*/
        
        for (unsigned int i = 0; i < PIPE_WIDTH; i++)
        {

            if (p->SC_latch[i].valid)
            {
                p->EX_latch[i] = p->SC_latch[i];
                p->SC_latch[i].valid = false;
            }
        }
        return;
    }

    // Otherwise, we need to handle multi-cycle instructions with EXEQ.

    // All valid entries from the SC latches are inserted into the EXEQ.
    for (unsigned int i = 0; i < PIPE_WIDTH; i++)
    {
        if (p->SC_latch[i].valid)
        {
            if (!exeq_insert(p->exeq, p->SC_latch[i].inst))
            {
                fprintf(stderr, "Error: EXEQ full\n");
                p->halt = true;
                return;
            }

            p->SC_latch[i].valid = false;
        }
    }

    // Cycle the EXEQ to reduce wait time for each instruction by 1 cycle.
    exeq_cycle(p->exeq);

    // Transfer all finished entries from the EXEQ to the EX latch.
    for (unsigned int i = 0; i < MAX_WRITEBACKS && exeq_check_done(p->exeq); i++)
    {
        p->EX_latch[i].valid = true;
        p->EX_latch[i].stall = false;
        p->EX_latch[i].inst = exeq_remove(p->exeq);
    }
}

/**
 * Simulate one cycle of the issue stage of a pipeline: insert decoded
 * instructions into the ROB and perform register renaming.
 * 
 * You must implement this function in pipeline.cpp in part B of the
 * assignment.
 * 
 * @param p the pipeline to simulate
 */
void pipe_cycle_issue(Pipeline *p)
{
    // TODO: For each valid instruction from the ID stage:

    // TODO: If there is space in the ROB, insert the instruction.
    // TODO: Set the entry invalid in the previous latch when you do so.

    // TODO: Then, check RAT for this instruction's source operands:
    // TODO: If src1 is not remapped, mark src1 as ready.
    // TODO: If src1 is remapped, set src1 tag accordingly, and set src1 ready
    //       according to whether the ROB entry with that tag is ready.
    // TODO: If src2 is not remapped, mark src2 as ready.
    // TODO: If src2 is remapped, set src2 tag accordingly, and set src2 ready
    //       according to whether the ROB entry with that tag is ready.

    // TODO: Set the tag for this instruction's destination register.
    // TODO: If this instruction writes to a register, update the RAT
    //       accordingly.
    #ifdef DEBUG
        printf("Issuing! Attempting to add entries to the ROB...\n");
    #endif

    // sorting the pipeline latch to issue older instruction first
    for(unsigned int i=0; i<PIPE_WIDTH - 1; i++){
        for(unsigned int j=i+1; j<PIPE_WIDTH; j++){
            if(p->ID_latch[j].inst.inst_num < p->ID_latch[i].inst.inst_num){
                InstInfo temp = p->ID_latch[j].inst;
                p->ID_latch[j].inst = p->ID_latch[i].inst;
                p->ID_latch[i].inst = temp;
            }
        }
    }

    for(unsigned int i=0; i<PIPE_WIDTH; i++){
        #ifdef DEBUG
            printf("\tChecking if we can issue on pipeline %d...\n", i);
        #endif
        if(p->ID_latch[i].valid){ // TODO: For each valid instruction from the ID stage:

            if(rob_check_space(p->rob)){ // TODO: If there is space in the ROB, insert the instruction.
                
                unsigned int cur_ind = p->rob->tail_ptr; // current location in ROB where instruction is inserted
                int rob_id = rob_insert(p->rob, p->ID_latch[i].inst);
                p->ID_latch[i].valid = false; // TODO: Set the entry invalid in the previous latch when you do so.

                #ifdef DEBUG
                    printf("\t\tAllocating entry for %lu in the ROB! rob_id: %d\n", p->ID_latch[i].inst.inst_num, cur_ind);
                #endif
                
                // TODO: Then, check RAT for this instruction's source operands:
                if(p->rob->entries[cur_ind].inst.src1_reg != -1){
                    if(!p->rat->entries[p->rob->entries[cur_ind].inst.src1_reg].valid){ // TODO: If src1 is not remapped, mark src1 as ready.
                        p->rob->entries[cur_ind].inst.src1_ready = true;
                        #ifdef DEBUG
                            printf("\t\tsrc_reg1 has no alias in the RAT! Ready by default.\n");
                        #endif
                    }
                    else{ // TODO: If src1 is remapped, set src1 tag accordingly, and set src1 ready
                        int _src1_tag = p->rat->entries[p->rob->entries[cur_ind].inst.src1_reg].prf_id;
                        p->rob->entries[cur_ind].inst.src1_tag = _src1_tag;
                        #ifdef DEBUG
                            printf("\t\tFound src_reg1 alias in RAT! Tag: %d, ready: %d\n", _src1_tag, p->rob->entries[_src1_tag].ready);
                        #endif
                        if(p->rob->entries[_src1_tag].ready){ // according to whether the ROB entry with that tag is ready.
                            p->rob->entries[cur_ind].inst.src1_ready = true; // doubtful
                            #ifdef DEBUG
                                printf("\t\tsrc_reg1 is not a needed register! Ready by default.\n");
                            #endif
                        }
                    }
                } 
                else{
                    #ifdef DEBUG
                        printf("\t\tsrc_reg1 is not a needed register! Ready by default.\n");
                    #endif
                    p->rob->entries[cur_ind].inst.src1_ready = true;
                }  

                if(p->rob->entries[cur_ind].inst.src2_reg != -1){
                    if(!p->rat->entries[p->rob->entries[cur_ind].inst.src2_reg].valid){ // TODO: If src2 is not remapped, mark src2 as ready.
                        p->rob->entries[cur_ind].inst.src2_ready = true;
                        #ifdef DEBUG
                            printf("\t\tsrc_reg2 has no alias in the RAT! Ready by default.\n");
                        #endif
                    }
                    else{ // TODO: If src2 is remapped, set src2 tag accordingly, and set src2 ready
                        int _src2_tag = p->rat->entries[p->rob->entries[cur_ind].inst.src2_reg].prf_id;
                        p->rob->entries[cur_ind].inst.src2_tag = _src2_tag;
                        #ifdef DEBUG
                            printf("\t\tFound src_reg2 alias in RAT! Tag: %d, ready: %d\n", _src2_tag, p->rob->entries[_src2_tag].ready);
                        #endif
                        if(p->rob->entries[_src2_tag].ready){ // according to whether the ROB entry with that tag is ready.
                            p->rob->entries[cur_ind].inst.src2_ready = true; // doubtful
                            #ifdef DEBUG
                                printf("\t\tsrc_reg2 is not a needed register! Ready by default.\n");
                            #endif
                        }
                    }
                }
                else{
                    #ifdef DEBUG
                        printf("\t\tsrc_reg2 is not a needed register! Ready by default.\n");
                    #endif
                    p->rob->entries[cur_ind].inst.src2_ready = true;
                }

                // TODO: Set the tag for this instruction's destination register.
                
                if(p->rob->entries[rob_id].inst.dest_reg != -1){
                    #ifdef DEBUG
                        printf("\t\tCreating an alias in the RAT: %d --> %d\n", p->rob->entries[rob_id].inst.dest_reg, rob_id);
                    #endif
                    rat_set_remap(p->rat, p->rob->entries[rob_id].inst.dest_reg, rob_id);
                    p->rob->entries[rob_id].inst.dr_tag = rob_id;
                }
                else{
                    p->rob->entries[rob_id].inst.dr_tag = rob_id; //points to itself as there is no destination
                }
                //p->rob->entries[rob_id].inst.dest_reg = rob_id; //check
                
            }

            else{
                #ifdef DEBUG
                    printf("\t\tUnable to add to to pipeline %d! Reason: No space in ROB!\n", i);
                    break;
                #endif
            }
        }
        else{
            #ifdef DEBUG
                printf("\t\tUnable to add to to pipeline %d! Reason: Instruction is not valid!\n", i);
            #endif
        }
        
    }      
}

/**
 * Simulate one cycle of the scheduling stage of a pipeline: schedule
 * instructions to execute if they are ready.
 * 
 * You must implement this function in pipeline.cpp in part B of the
 * assignment.
 * 
 * @param p the pipeline to simulate
 */
void pipe_cycle_schedule(Pipeline *p)
{
    // TODO: Implement two scheduling policies:
    
    if (SCHED_POLICY == SCHED_IN_ORDER)
    {
        #ifdef DEBUG
            printf("Scheduling! Policy: In-Order\n");
        #endif
        for(unsigned int j=0; j< PIPE_WIDTH; j++){
            // In-order scheduling:
            // TODO: Find the oldest valid entry in the ROB that is not already
            //       executing.
            #ifdef DEBUG
                printf("\tChecking if we can schedule in pipeline %d...\n", j);
            #endif
            int oldest_inst_entry = -1;
            InstInfo oldest_inst_info;
            if(p->rob->head_ptr < p->rob->tail_ptr){ // if entries in rob is within the max limit
                for(int i = p->rob->head_ptr; i < p->rob->tail_ptr; i++){
                    if(p->rob->entries[i].valid && !p->rob->entries[i].exec){
                        oldest_inst_info = p->rob->entries[i].inst;
                        oldest_inst_entry = i; // entry ID in rob
                        break;
                    }
                }
            }
            else if(p->rob->head_ptr >= p->rob->tail_ptr) // if entries in rob is beyond the max limit
            {
                int found_flag = 0;
                for(unsigned int i = p->rob->head_ptr; i < NUM_ROB_ENTRIES; i++)
                {
                    if(p->rob->entries[i].valid && !p->rob->entries[i].exec){
                        oldest_inst_info = p->rob->entries[i].inst;
                        oldest_inst_entry = i; // entry ID in rob
                        found_flag = 1;
                        break;
                    }
                }

                if(found_flag == 0){
                    for(int i = 0; i < p->rob->tail_ptr; i++)
                    {
                        if(p->rob->entries[i].valid && !p->rob->entries[i].exec){
                            oldest_inst_info = p->rob->entries[i].inst;
                            oldest_inst_entry = i; // entry ID in rob
                            break;
                        }
                    }
                }
                
            }
        
            /*for(unsigned int i=0; i<NUM_ROB_ENTRIES; i++){
                if(p->rob->entries[i].valid && !p->rob->entries[i].exec){
                    if(oldest_inst_entry == -1){
                        oldest_inst_info = p->rob->entries[i].inst;
                        oldest_inst_entry = i; // entry ID in rob
                    }
                    else if(p->rob->entries[i].inst.inst_num < (uint64_t)oldest_inst_entry){
                        oldest_inst_info = p->rob->entries[i].inst;
                        oldest_inst_entry = i; // entry ID in rob
                    }
                }
            }*/
            if(oldest_inst_entry != -1){
                #ifdef DEBUG
                    printf("\t\t%lu can possibly execute! Checking if it can go...\n", p->rob->entries[oldest_inst_entry].inst.inst_num);// (uint64_t)oldest_inst_entry);
                #endif

                // TODO: Check if it is stalled, i.e., if at least one source operand
                //       is not ready.
                // TODO: If so, stop scheduling instructions.
                // TODO: Otherwise, mark it as executing in the ROB and send it to the
                //       next latch.
                if(p->rob->entries[oldest_inst_entry].inst.src1_ready and p->rob->entries[oldest_inst_entry].inst.src2_ready){
                    #ifdef DEBUG
                        printf("\t\tBoth src registers ready! Executing %lu!\n", p->rob->entries[oldest_inst_entry].inst.inst_num);
                    #endif
                    p->rob->entries[oldest_inst_entry].exec = true;
                    p->SC_latch[j].inst = p->rob->entries[oldest_inst_entry].inst;
                    p->SC_latch[j].valid = true;
                }  
                else{
                    #ifdef DEBUG
                        printf("\t\t%lu couldn't execute (src1_ready: %d, src2_ready: %d)!\n", (uint64_t)oldest_inst_entry, p->rob->entries[oldest_inst_entry].inst.src1_ready, p->rob->entries[oldest_inst_entry].inst.src2_ready);
                    #endif
                }

            }
            else{
                #ifdef DEBUG
                    printf("\t\tCouldn't find any instructions to execute!\n");
                #endif
            }
        }  
            
            // TODO: Repeat for each lane of the pipeline.
            /*if(oldest_inst_entry == -1){
                #ifdef DEBUG
                    printf("\t\tItereated through the whole ROB and couldn't find anything to execute!\n");
                #endif
            }*/
    }
        
    if (SCHED_POLICY == SCHED_OUT_OF_ORDER){
        // Out-of-order scheduling:
        // TODO: Find the oldest valid entry in the ROB that has both source
        //       operands ready but is not already executing.
        // TODO: Mark it as executing in the ROB and send it to the next latch.
        // TODO: Repeat for each lane of the pipeline.
        #ifdef DEBUG
            printf("Scheduling! Policy: Out-of-Order\n");
        #endif
        for(unsigned int j=0; j< PIPE_WIDTH; j++){
            // In-order scheduling:
            // TODO: Find the oldest valid entry in the ROB that is not already
            //       executing.
            #ifdef DEBUG
                printf("\tChecking if we can schedule in pipeline %d...\n", j);
            #endif
            int oldest_inst_entry = -1;
            InstInfo oldest_inst_info;
            //int repeat_flag = 1;
            uint64_t cur_ptr = p->rob->head_ptr;
            int found_flag = 0;
            if(p->rob->head_ptr < p->rob->tail_ptr){
                while(found_flag == 0 && cur_ptr < (uint64_t)p->rob->tail_ptr){
                    if(p->rob->entries[cur_ptr].valid && !p->rob->entries[cur_ptr].exec){
                        oldest_inst_info = p->rob->entries[cur_ptr].inst;
                        oldest_inst_entry = cur_ptr; // entry ID in rob

                        #ifdef DEBUG
                            printf("\t\t%lu can possibly execute! Checking if it can go...\n", p->rob->entries[oldest_inst_entry].inst.inst_num);// (uint64_t)oldest_inst_entry);
                        #endif
                        
                        if(p->rob->entries[oldest_inst_entry].inst.src1_ready and p->rob->entries[oldest_inst_entry].inst.src2_ready){
                            #ifdef DEBUG
                                printf("\t\tBoth src registers ready! Executing %lu!\n", p->rob->entries[oldest_inst_entry].inst.inst_num);
                            #endif
                            p->rob->entries[oldest_inst_entry].exec = true;
                            p->SC_latch[j].inst = p->rob->entries[oldest_inst_entry].inst;
                            p->SC_latch[j].valid = true;
                            found_flag = 1;
                            break;
                        }  
                        else{
                            #ifdef DEBUG
                                printf("\t\t%lu couldn't execute (src1_ready: %d, src2_ready: %d)!\n", (uint64_t)oldest_inst_entry, p->rob->entries[oldest_inst_entry].inst.src1_ready, p->rob->entries[oldest_inst_entry].inst.src2_ready);
                            #endif
                            
                        }
                    }
                    cur_ptr++;
                }
            }
            else if(found_flag == 0 and p->rob->head_ptr >= p->rob->tail_ptr){
                while(found_flag == 0 && cur_ptr < NUM_ROB_ENTRIES){
                    if(p->rob->entries[cur_ptr].valid && !p->rob->entries[cur_ptr].exec){
                        oldest_inst_info = p->rob->entries[cur_ptr].inst;
                        oldest_inst_entry = cur_ptr; // entry ID in rob

                        #ifdef DEBUG
                            printf("\t\t%lu can possibly execute! Checking if it can go...\n", p->rob->entries[oldest_inst_entry].inst.inst_num);// (uint64_t)oldest_inst_entry);
                        #endif
                        
                        if(p->rob->entries[oldest_inst_entry].inst.src1_ready and p->rob->entries[oldest_inst_entry].inst.src2_ready){
                            #ifdef DEBUG
                                printf("\t\tBoth src registers ready! Executing %lu!\n", p->rob->entries[oldest_inst_entry].inst.inst_num);
                            #endif
                            p->rob->entries[oldest_inst_entry].exec = true;
                            p->SC_latch[j].inst = p->rob->entries[oldest_inst_entry].inst;
                            p->SC_latch[j].valid = true;
                            found_flag = 1;
                            break;
                        }  
                        else{
                            #ifdef DEBUG
                                printf("\t\t%lu couldn't execute (src1_ready: %d, src2_ready: %d)!\n", (uint64_t)oldest_inst_entry, p->rob->entries[oldest_inst_entry].inst.src1_ready, p->rob->entries[oldest_inst_entry].inst.src2_ready);
                            #endif
                            
                        }
                    }
                    cur_ptr++;
                }
                cur_ptr = 0;
                while(found_flag == 0 && cur_ptr < (uint64_t)p->rob->tail_ptr){
                    if(p->rob->entries[cur_ptr].valid && !p->rob->entries[cur_ptr].exec){
                        oldest_inst_info = p->rob->entries[cur_ptr].inst;
                        oldest_inst_entry = cur_ptr; // entry ID in rob

                        #ifdef DEBUG
                            printf("\t\t%lu can possibly execute! Checking if it can go...\n", p->rob->entries[oldest_inst_entry].inst.inst_num);// (uint64_t)oldest_inst_entry);
                        #endif
                        
                        if(p->rob->entries[oldest_inst_entry].inst.src1_ready and p->rob->entries[oldest_inst_entry].inst.src2_ready){
                            #ifdef DEBUG
                                printf("\t\tBoth src registers ready! Executing %lu!\n", p->rob->entries[oldest_inst_entry].inst.inst_num);
                            #endif
                            p->rob->entries[oldest_inst_entry].exec = true;
                            p->SC_latch[j].inst = p->rob->entries[oldest_inst_entry].inst;
                            p->SC_latch[j].valid = true;
                            found_flag = 1;
                            break;
                        }  
                        else{
                            #ifdef DEBUG
                                printf("\t\t%lu couldn't execute (src1_ready: %d, src2_ready: %d)!\n", (uint64_t)oldest_inst_entry, p->rob->entries[oldest_inst_entry].inst.src1_ready, p->rob->entries[oldest_inst_entry].inst.src2_ready);
                            #endif
                            
                        }
                    }
                    cur_ptr++;
                }
            }

            if(oldest_inst_entry == -1){
                #ifdef DEBUG
                    printf("\t\tCouldn't find any instructions to execute!\n");
                #endif

            }
        }  

    }

    /*if (SCHED_POLICY == SCHED_OUT_OF_ORDER)
    {
        // Out-of-order scheduling:
        // TODO: Find the oldest valid entry in the ROB that has both source
        //       operands ready but is not already executing.
        // TODO: Mark it as executing in the ROB and send it to the next latch.
        // TODO: Repeat for each lane of the pipeline.
        #ifdef DEBUG
            printf("Scheduling! Policy: Out-of-Order\n");
        #endif
        for(unsigned int j=0; j<PIPE_WIDTH; j++){
            #ifdef DEBUG
                printf("\tChecking if we can schedule in pipeline %d...\n", j);
            #endif
            int oldest_inst_entry = -1;
            InstInfo oldest_inst_info;
            for(int i=0; i<MAX_ROB_ENTRIES; i++){
                #ifdef DEBUG
                    printf("\t\t%lu can possibly execute! Checking if it can go...\n", (uint64_t)i);
                #endif
                if(p->rob->entries[i].inst.inst_num < (uint64_t)oldest_inst_entry && !p->rob->entries[i].exec 
                && p->rob->entries[i].inst.src1_ready && p->rob->entries[i].inst.src2_ready){
                    oldest_inst_entry = i;
                    oldest_inst_info = p->rob->entries[i].inst;
                }
                else{
                    #ifdef DEBUG
                        printf("\t\t%lu couldn't execute (src1_ready: %d, src2_ready: %d)!\n", (uint64_t)oldest_inst_entry, p->rob->entries[i].inst.src1_ready, p->rob->entries[i].inst.src2_ready);
                    #endif
                }
                
            }
            #ifdef DEBUG
                printf("\t\tBoth src registers ready! Executing %lu!\n", (uint64_t)oldest_inst_entry);
            #endif
            p->rob->entries[oldest_inst_entry].exec = true;
            p->SC_latch[j].inst = p->rob->entries[oldest_inst_entry].inst;
            p->SC_latch[j].valid = true;

            if(oldest_inst_entry == -1){
                #ifdef DEBUG
                    printf("\t\tItereated through the whole ROB and couldn't find anything to execute!\n");
                #endif
            }
        }
    }*/
}

/**
 * Simulate one cycle of the writeback stage of a pipeline: update the ROB
 * with information from instructions that have finished executing.
 * 
 * You must implement this function in pipeline.cpp in part B of the
 * assignment.
 * 
 * @param p the pipeline to simulate
 */
void pipe_cycle_writeback(Pipeline *p)
{
    // TODO: For each valid instruction from the EX stage:
    // TODO: Broadcast the result to all ROB entries.
    // TODO: Update the ROB: mark the instruction ready to commit.
    // TODO: Invalidate the instruction in the previous latch.

    // Remember: how many instructions can the EX stage send to the WB stage
    // in one cycle?

    #ifdef DEBUG
        printf("Writeback! Attempting to writeback instructions...\n");
    #endif

    for(int i=0; i<MAX_WRITEBACKS; i++){
        if(p->EX_latch[i].valid){
            rob_wakeup(p->rob, p->EX_latch[i].inst.dr_tag);
            rob_mark_ready(p->rob, p->EX_latch[i].inst);
            p->EX_latch[i].valid = false;
            
            #ifdef DEBUG
                printf("\tWriting back %lu! Sending broadcast on tag %d!\n", p->EX_latch[i].inst.inst_num, p->EX_latch[i].inst.dr_tag); 
            #endif
        }
        else{
            #ifdef DEBUG
                printf("\tEX latch #%lu is invalid! Can no longer writeback!\n", (uint64_t)i); 
            #endif
            break;
        }
    }

}

/**
 * Simulate one cycle of the commit stage of a pipeline: commit instructions
 * in the ROB that are ready to commit.
 * 
 * You must implement this function in pipeline.cpp in part B of the
 * assignment.
 * 
 * @param p the pipeline to simulate
 */
void pipe_cycle_commit(Pipeline *p)
{
    // TODO: Check if the instruction at the head of the ROB is ready to
    //       commit.
    // TODO: If so, remove it from the ROB.
    // TODO: Commit that instruction.
    // TODO: If a RAT mapping exists and is still relevant, update the RAT.
    // TODO: Repeat for each lane of the pipeline.

    // The following code is DUMMY CODE to ensure that the base code compiles
    // and that the simulation terminates. Replace it with a correct
    // implementation!

    #ifdef DEBUG
        printf("Committing! Attempting to commit instructions...\n");
    #endif
    
    for(unsigned int i=0; i<PIPE_WIDTH; i++){
        InstInfo cur_inst;
        if(rob_check_head(p->rob)){
            #ifdef DEBUG
                printf("\tCommitting %lu!\n", (uint64_t)p->rob->entries[p->rob->head_ptr].inst.inst_num);
            #endif
            cur_inst = rob_remove_head(p->rob);
            pipe_commit_inst(p, cur_inst);

            if(p->rat->entries[cur_inst.dest_reg].valid && p->rat->entries[cur_inst.dest_reg].prf_id == (uint64_t)cur_inst.dr_tag){
                p->rat->entries[cur_inst.dest_reg].valid = false;
                #ifdef DEBUG
                    printf("\tMapping in RAT between R%d and rob id %d exists. Resetting RAT entry!\n", cur_inst.dest_reg, cur_inst.dr_tag);
                #endif
            }
        }
        else{
            #ifdef DEBUG
                printf("\tNothing to commit!\n");
            #endif
        }
    }
    /*for (unsigned int i = 0; i < PIPE_WIDTH; i++)
    {
        if (p->FE_latch[i].valid)
        {
            pipe_commit_inst(p, p->FE_latch[i].inst);
            p->FE_latch[i].valid = false;
        }
    }*/
}
