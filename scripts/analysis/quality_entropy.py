#!/usr/bin/env python3
import numpy as np
import struct

readlen = 147
inFile = "NA12878-Rep-1_S1_L001_R1_counts"
num_clusters = 1

def quality_to_prob(qual_string):
    _q = [ord(c) for c in qual_string]

    _q = (33.0 - np.array(_q))/10.0
    prob = np.power(10.0,_q)
    return prob

def get_M_readlen(inFile):
    M = 0
    N = 0
    with open(inFile,'r') as f:
      for line in f:
          M += 1
          N = len(line) -1
    return (M,N)


def qv_to_prob():
    _q  = -np.arange(42)/10.0 
    prob = np.reshape(np.power(10.0,_q),[1,42])
    return prob


def quality_value_stats(inFile):
    qv_counts_0order = np.zeros((42,readlen))
    qv_counts_1order = np.zeros((42,42,readlen))
    qv_counts_2order = np.zeros((42,42,42,readlen))
    
    qv_counts_0order_cluster = np.zeros((42,readlen,num_clusters))
    qv_counts_1order_cluster = np.zeros((42,42,readlen,num_clusters))
    qv_counts_2order_cluster = np.zeros((42,42,42,readlen,num_clusters))

    
    qv_prob_0order_cluster = np.zeros((42,readlen,num_clusters))
    qv_prob_1order_cluster = np.zeros((42,42,readlen,num_clusters))
    qv_prob_2order_cluster = np.zeros((42,42,42,readlen,num_clusters))
    f = open(inFile,'r')

    num_reads_cluster = np.zeros((num_clusters))
    for cluster in range(num_clusters):    
        s = f.read(8)
        num_reads_cluster[cluster] = struct.unpack('Q',s)[0]
        for i in range(readlen):
            for j in range(42):
                s = f.read(8)
                qv_counts_0order_cluster[j,i,cluster] = struct.unpack('Q',s)[0] 
        for i in range(readlen-1):
            for j in range(42):
                for k in range(42):
                    s = f.read(8)
                    qv_counts_1order_cluster[j,k,i,cluster] = struct.unpack('Q',s)[0] 
        for i in range(readlen-2):
            for j in range(42):
                for k in range(42):
                    for l in range(42):
                        s = f.read(8)
                        qv_counts_2order_cluster[j,k,l,i,cluster] = struct.unpack('Q',s)[0] 

    
    num_reads = np.sum(num_reads_cluster)    
    qv_prob_0order = np.sum(qv_counts_0order_cluster,axis=2)/(1.0*num_reads)
    qv_prob_1order = np.sum(qv_counts_1order_cluster,axis=3)/(1.0*num_reads)
    qv_prob_2order = np.sum(qv_counts_2order_cluster,axis=4)/(1.0*num_reads)

    for cluster in range(num_clusters):    
        qv_prob_0order_cluster[:,:,cluster] = qv_counts_0order_cluster[:,:,cluster]/(1.0*num_reads_cluster[cluster])
        qv_prob_1order_cluster[:,:,:,cluster] = qv_counts_1order_cluster[:,:,:,cluster]/(1.0*num_reads_cluster[cluster])
        qv_prob_2order_cluster[:,:,:,:,cluster] = qv_counts_2order_cluster[:,:,:,:,cluster]/(1.0*num_reads_cluster[cluster])
    
    
    return (qv_prob_0order, qv_prob_1order, qv_prob_2order,
            qv_prob_0order_cluster, qv_prob_1order_cluster, qv_prob_2order_cluster,
            num_reads,num_reads_cluster)


def avg_quality_value(inFile):
    num_reads,readlen = get_M_readlen(inFile)
    qv_avg = np.zeros(num_reads)
    with open(inFile,'r') as f_qv:
        for i,qv in tqdm(enumerate(f_qv),total=num_reads,desc="computing read probs ...",ascii=True):
            qv_id = qv.rstrip('\n')
            qv_id = [ord(c) - 33.0 for c in qv_id]
            qv_avg[i] = np.mean(qv_id)
    return qv_avg



def compute_Ni_old_entropy(probs):
    _e = -( probs*np.log(probs) + (1-probs)*np.log(1-probs) ) + probs*np.log(3.0)  
    _e = _e/np.log(2.0)
    entropy = np.sum(_e)
    return entropy,_e 

def compute_Ni_entropy(probs):
    _e = -( probs*np.log(probs) + (1-probs)*np.log(1-probs)  + probs*(0.3*np.log(0.15)+0.7*np.log(0.7)))
    _e = _e/np.log(2.0)
    entropy = np.sum(_e)
    return entropy,_e 

def compute_Si_prob(qv_prob):
    prob_substitution = np.dot(qv_to_prob(),qv_prob)
    return prob_substitution



def xlogx(p):
    _e = p*np.log(p)
    _e = _e/np.log(2.0)
    return _e


def compute_N1N2_old_entropy(qv_N1N2_probs):
    qv_to_prob_0 = 1.0 - qv_to_prob()
    qv_to_prob_1 = qv_to_prob()
    
    temp_prob_00 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_0))
    temp_prob_01 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_1))/3.0
    temp_prob_10 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_0))/3.0
    temp_prob_11 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_1))/9.0
    entropy = -(xlogx(temp_prob_00) + 3.0*xlogx(temp_prob_01) + 3.0*xlogx(temp_prob_10) + 9.0*xlogx(temp_prob_11))
    return entropy

def compute_N1N2N3_entropy(qv_N1N2N3_probs):
    qv_to_prob_0 = 1.0 - qv_to_prob()
    qv_to_prob_1 = qv_to_prob()
    qv_2_prob = np.stack((qv_to_prob_0, qv_to_prob_1))
    noise_entropy = -(0.3*np.log(0.15)+0.7*np.log(0.7))/np.log(2.0)

    entropy = 0
    
    def prob_S1S2S3(s1,s2,s3):
        temp_prob = 0;
        for i0 in range(42):
            for i1 in range(42):
                for i2 in range(42):
                    temp_prob += qv_N1N2N3_probs[i0,i1,i2]*qv_2_prob[s1,0,i0]*qv_2_prob[s2,0,i1]*qv_2_prob[s3,0,i2]
        return temp_prob
    
    prob_s1s2s3 = np.zeros((2,2,2))
    for s1 in [0,1]:
        for s2 in [0,1]:
            for s3 in [0,1]:
                prob_s1s2s3[s1,s2,s3] = prob_S1S2S3(s1,s2,s3)
                entropy += (-xlogx(prob_s1s2s3[s1,s2,s3]))
    prob_s1 = np.sum(prob_s1s2s3[1,:,:])
    prob_s2 = np.sum(prob_s1s2s3[:,1,:])
    prob_s3 = np.sum(prob_s1s2s3[:,:,1])
    entropy += (prob_s1 + prob_s2 + prob_s3)*noise_entropy
    
    return entropy
    
                

def compute_N1N2_entropy(qv_N1N2_probs):
    qv_to_prob_0 = 1.0 - qv_to_prob()
    qv_to_prob_1 = qv_to_prob()
    noise_entropy = -(0.3*np.log(0.15)+0.7*np.log(0.7))/np.log(2.0)
    
    temp_prob_00 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_0))
    temp_prob_01 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_1))
    temp_prob_10 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_0))
    temp_prob_11 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_1))
    p_S1_1 = temp_prob_10 + temp_prob_11
    p_S2_1 = temp_prob_01 + temp_prob_11
    entropy = -(xlogx(temp_prob_00) + xlogx(temp_prob_01) + xlogx(temp_prob_10) + xlogx(temp_prob_11))
    entropy += (p_S1_1 + p_S2_1)*noise_entropy
    return entropy
    
def compute_Ni_joint_probability(qv_joint_probs,Ni_perpos_entropy):
    readlen = Ni_perpos_entropy.shape[1]
    entropy = Ni_perpos_entropy[0,0] 
    perpos_1order_entropy = np.zeros(readlen-1)
    for i in range(readlen-1):
        qv_N1N2_probs = qv_joint_probs[:,:,i]
        perpos_1order_entropy[i] = compute_N1N2_entropy(qv_N1N2_probs)
        entropy += perpos_1order_entropy[i] - Ni_perpos_entropy[0,i]
    return entropy,perpos_1order_entropy

def compute_2order_entropy(qv_prob_2order, perpos_1order_entropy):
    readlen = perpos_1order_entropy.shape[0] + 1
    entropy = perpos_1order_entropy[0]
    
    for i in range(readlen-2):
        qv_N1N2N3_probs = qv_prob_2order[:,:,:,i]
        entropy += compute_N1N2N3_entropy(qv_N1N2N3_probs) - perpos_1order_entropy[i]
    return entropy


def compute_order_entropies(qv_prob_0order, qv_prob_1order, qv_prob_2order):
    l0 = np.sum(np.nan_to_num(-qv_prob_0order * np.log2(qv_prob_0order)), axis=0)
    l1 = np.sum(np.nan_to_num(-qv_prob_1order * np.log2(qv_prob_1order)), axis=(0, 1))
    l2 = np.sum(
        np.nan_to_num(-qv_prob_2order * np.log2(qv_prob_2order)),
        axis=(0, 1, 2),
    )

    zero_order_entropy = np.sum(l0)
    first_order_entropy = np.sum(l1) - np.sum(l0) + l0[0]
    second_order_entropy = np.sum(l2) - np.sum(l1) + l1[0]
    return zero_order_entropy, first_order_entropy, second_order_entropy


def print_order_entropies(zero_order_entropy, first_order_entropy,
                          second_order_entropy):
    print('Zero order', zero_order_entropy)
    print('First order', first_order_entropy)
    print('Second order', second_order_entropy)


def main():
    (qv_prob_0order, qv_prob_1order, qv_prob_2order,
     qv_prob_0order_cluster, qv_prob_1order_cluster,
     qv_prob_2order_cluster, num_reads,
     num_reads_cluster) = quality_value_stats(inFile)

    del qv_prob_0order_cluster
    del qv_prob_1order_cluster
    del qv_prob_2order_cluster
    del num_reads
    del num_reads_cluster

    # Summarize empirical 0th-, 1st-, and 2nd-order quality entropies.
    zero_order_entropy, first_order_entropy, second_order_entropy = (
        compute_order_entropies(qv_prob_0order, qv_prob_1order, qv_prob_2order)
    )
    print_order_entropies(
        zero_order_entropy,
        first_order_entropy,
        second_order_entropy,
    )

if __name__ == '__main__':
    main()
