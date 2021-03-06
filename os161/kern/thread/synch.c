/*
 * Synchronization primitives.
 * See synch.h for specifications of the functions.
 */

#include <types.h>
#include <lib.h>
#include <synch.h>
#include <thread.h>
#include <curthread.h>
#include <machine/spl.h>
#include <queue.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *namearg, int initial_count)
{
	struct semaphore *sem;

	assert(initial_count >= 0);

	sem = kmalloc(sizeof(struct semaphore));
	if (sem == NULL) {
		return NULL;
	}

	sem->name = kstrdup(namearg);
	if (sem->name == NULL) {
		kfree(sem);
		return NULL;
	}

	sem->count = initial_count;
	return sem;
}

void
sem_destroy(struct semaphore *sem)
{
	int spl;
	assert(sem != NULL);

	spl = splhigh();
	assert(thread_hassleepers(sem)==0);
	splx(spl);

	/*
	 * Note: while someone could theoretically start sleeping on
	 * the semaphore after the above test but before we free it,
	 * if they're going to do that, they can just as easily wait
	 * a bit and start sleeping on the semaphore after it's been
	 * freed. Consequently, there's not a whole lot of point in 
	 * including the kfrees in the splhigh block, so we don't.
	 */

	kfree(sem->name);
	kfree(sem);
}

void 
P(struct semaphore *sem)
{
	int spl;
	assert(sem != NULL);

	/*
	 * May not block in an interrupt handler.
	 *
	 * For robustness, always check, even if we can actually
	 * complete the P without blocking.
	 */
	assert(in_interrupt==0);

	spl = splhigh(); //Disable All Interrupts
	while (sem->count==0) { //Check to see if there are any resources. If not, sleep.
		thread_sleep(sem);
	}
	assert(sem->count>0);  //When there are resources...
	sem->count--;  //Decrease by 1 to say that it was acquired
	splx(spl); //Enable All Interrupts
}

void
V(struct semaphore *sem)
{
	int spl;
	assert(sem != NULL);
	spl = splhigh(); //Disable all Interrupts
	sem->count++; //Release 1 resources
	assert(sem->count>0); 
	thread_wakeup(sem); //Wakeup threads that were sleeping on event count == 0
	splx(spl); // Enable Interrupts
}

////////////////////////////////////////////////////////////
//
// Lock.


struct lock *
lock_create(const char *name)
{
	struct lock *lock; 

	lock = kmalloc(sizeof(struct lock));
	if (lock == NULL) {
		return NULL;
	}

	lock->name = kstrdup(name);
	if (lock->name == NULL) {
		kfree(lock);
		return NULL;
	}
	
	// Initialize Struct lock
    lock->lock_occupied = 0;  //lock currently not held
	lock->lock_holder = NULL; //currently no holder
	return lock;
}

void
	lock_destroy(struct lock *lock)
{
	assert(lock != NULL);

	kfree(lock->name);
	kfree(lock);
	lock = NULL; //set struct to null just in case.
}

void
lock_acquire(struct lock *lock)
{	

	//We must first disable interrupts
	int spl;
	assert(lock != NULL);

	//Do the same check that we do w/ semaphores such that we do not disable interrupts while being potentially in the interrupt handler.
	//assert(in_interrupt==0);

	spl = splhigh(); //Disable All Interrupts
	while (lock->lock_occupied == 1 && lock->lock_holder != curthread){
		//If the lock is being held by another thread then we must sleep.
		thread_sleep(lock);
	}

	//We can now take the lock!
	lock->lock_occupied =1;
	lock->lock_holder = curthread;

	//Re-enable all interrupts.
	splx(spl);

	(void)lock;  // suppress warning until code gets written
}

void
lock_release(struct lock *lock)
{
	int spl;
	assert(lock !=NULL);

	spl = splhigh();
	
	//Give up the lock
	lock->lock_occupied = 0;
	lock->lock_holder = NULL;
	
	//Wakeup the next thread waiting for the lock
	thread_wakeup(lock);

	//Re-enable all interrupts.
	splx(spl);

	(void)lock;  // suppress warning until code gets written
}

int
lock_do_i_hold(struct lock *lock)
{
	return (lock->lock_holder == curthread); //returns true/false

	(void)lock;  // suppress warning until code gets written
}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
	struct cv *cv;

	cv = kmalloc(sizeof(struct cv));
	if (cv == NULL) {
		return NULL;
	}

	cv->name = kstrdup(name);
	if (cv->name==NULL) {
		kfree(cv);
		return NULL;
	}

	//Initialize count to 0
	cv->count=0;
	
	//Create the queue
	cv->thread_queue = q_create(1);
	
	return cv;
}

void
cv_destroy(struct cv *cv)
{
	assert(cv != NULL);
	
	//Like locks, before we destroy, we want to ensure there is no one waiting or in the queue.
	assert(cv->count == 0); // no resources left!
	assert(q_empty(cv->thread_queue)); //make sure the queue is empty
	
	//If those pass, we destroy the queue!
	q_destroy(cv->thread_queue);
	
	kfree(cv->name);
	kfree(cv);
	
	cv = NULL; //set the pointer to null just in case
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	int spl;
		
	//We must complete an unconditional wait once an unlock occurs and we can then take the lock. We will check the conditions now.
	assert(cv != NULL);
	assert(lock !=NULL);
	assert (lock_do_i_hold(lock));
	
	//If these steps above are valid we can now release the lock, sleep and then lock again.
	//This must be done atomically.
	
	//Like locks and semaphores, we want to make sure before we disable interrupts that we are not currently in the interrupt handler.
	assert(in_interrupt == 0);
	
	spl = splhigh(); //Disable All Interrupts
	
	lock_release(lock); //Unlock
	
	cv->count++; //Add one to the count since we have one more thread waiting now.
	
	q_preallocate(cv->thread_queue,cv->count); // not sure about this.
	
	q_addtail(cv->thread_queue, curthread); //add the currently waiting thread in the queue;
	
	thread_sleep(curthread); // now that the thread is in the queue, it can sleep.
	
	lock_acquire(lock); //When awoken, reacquire the lock if available. If not available, the thread will go back to bed inside lock_acquire();

	splx(spl); //Re-enable interrupts
	
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	//On a signal, this means the next thread in the queue can start!!
	
	int spl;
	//We must complete an unconditional wait once an unlock occurs and we can then take the lock. We will check the conditions now.
	assert(cv != NULL);
	assert(lock !=NULL);
	assert (lock_do_i_hold(lock));
	
	spl = splhigh(); //Disable All Interrupts
	
	cv->count--; //Decrement count since the next thread can go.
	
	//We will never know which thread is next, so we must create a temp thread pointer to be able to work with the next pointer in the queue.
	struct thread *next_thread = q_remhead(cv->thread_queue); //removes the next head in the queue.
    
	
	thread_wakeup(next_thread); //Wake up this next thread!
    
	splx(spl); //Re-enable All Interrupts
	
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	assert(cv != NULL);
	assert (lock != NULL);
	assert (lock_do_i_hold(lock));
	
	//Broadcast must wakeup all threads, so we use count to help us!
	while (cv->count > 0){
		
		cv_signal(cv, lock);
		//Since signal wakes up the next thread, and decrements count, this is all we need to do here.
	
	}
	
	//Make sure that we woke up all threads in the queue.
	assert(cv->count == 0);
	assert(q_empty(cv->thread_queue));
	
	(void)cv;    // suppress warning until code gets written
	(void)lock;  // suppress warning until code gets written
}
